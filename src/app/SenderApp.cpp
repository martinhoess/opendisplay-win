#include "app/SenderApp.h"

#include "app/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <utility>

#include <winsock2.h>

#include "display/DesktopDuplication.h"
#include "display/VirtualDisplay.h"
#include "encode/H264Encoder.h"
#include "input/InputInjector.h"
#include "net/Connection.h"
#include "net/Protocol.h"

namespace od {

namespace {

constexpr uint32_t kFps = 60;
constexpr uint32_t kBitrateBps = 30'000'000;
constexpr int kSendTimeoutMs = 500;      // backpressure: how long to wait for the socket before dropping a frame
constexpr int kReconnectDelayMs = 2000;
constexpr int kKeepaliveMs = 1000;       // max silence on a static screen; well under the iPad's ~5s watchdog
constexpr int kActiveTailMs = 300;       // keep feeding the encoder this long after the last change (drains its 1-frame hold)
constexpr int kWrongSizeGraceMs = 3000;  // how long the monitor may sit on a foreign size before we rebuild it
constexpr int kBlockedRetryMs = 5000;    // how often a waiting iPad checks whether the display is free again

// Only one panel size may be on the air at a time.
//
// parsec-vdd puts a single custom resolution on all of its virtual monitors:
// whichever sender sets its size last drags every other monitor along, and
// those iPads then show a letterboxed desktop at the wrong aspect. Rather than
// serve a wrong-shaped picture, iPads of the same panel size run together and
// a different one waits until the display is free again.
//
// Process-wide, because the tray drives every sender. A headless CLI sender
// started next to the tray is outside this and can still take the mode with
// it — that path is for testing.
struct PanelState {
    std::mutex mutex;
    uint32_t width = 0;
    uint32_t height = 0;
    int holders = 0;
};

PanelState& Panel()
{
    static PanelState state;
    return state;
}

// Takes a share of the display for w x h. Returns false when a different size
// holds it, and then reports that size in activeW/activeH.
bool AcquirePanel(uint32_t w, uint32_t h, uint32_t& activeW, uint32_t& activeH)
{
    PanelState& panel = Panel();
    std::lock_guard<std::mutex> lock(panel.mutex);
    if (panel.holders > 0 && (panel.width != w || panel.height != h)) {
        activeW = panel.width;
        activeH = panel.height;
        return false;
    }
    panel.width = w;
    panel.height = h;
    ++panel.holders;
    return true;
}

void ReleasePanel()
{
    PanelState& panel = Panel();
    std::lock_guard<std::mutex> lock(panel.mutex);
    if (--panel.holders <= 0) {
        panel.holders = 0;
        panel.width = 0;
        panel.height = 0;
    }
}

// Rotation: the panel is the same device, just turned. Only the sole holder
// may change the size under the claim — with another iPad attached the two
// would fight over the one custom resolution again.
bool RetunePanel(uint32_t w, uint32_t h)
{
    PanelState& panel = Panel();
    std::lock_guard<std::mutex> lock(panel.mutex);
    if (panel.holders > 1)
        return false;
    panel.width = w;
    panel.height = h;
    return true;
}

// Releases the claim when the connection ends, whichever way it ends.
struct PanelHolder {
    bool held = false;
    ~PanelHolder()
    {
        if (held)
            ReleasePanel();
    }
};

uint32_t EvenClamp(int32_t v, uint32_t fallback)
{
    uint32_t u = v > 0 ? static_cast<uint32_t>(v) : fallback;
    return u & ~1u; // NV12 4:2:0 needs even dimensions
}

} // namespace

SenderApp::~SenderApp()
{
    Stop();
}

void SenderApp::Start(std::string ip, uint16_t port)
{
    if (running_.exchange(true))
        return; // already running

    stopRequested_ = false;
    worker_ = std::thread([this, ip = std::move(ip), port] { RunLoop(ip, port); });
}

void SenderApp::Stop()
{
    stopRequested_ = true;

    // Unblock the worker if it's parked in Connect's socket / a blocking
    // ReadFrame: closing the socket makes those calls fail promptly.
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        if (activeConn_ != nullptr)
            activeConn_->Close();
    }

    if (worker_.joinable())
        worker_.join();

    running_ = false;
    stopRequested_ = false;
    state_ = State::Idle;
}

void SenderApp::RunBlocking(std::string ip, uint16_t port)
{
    running_ = true;
    RunLoop(std::move(ip), port);
    running_ = false;
}

void SenderApp::InterruptibleSleep(int ms)
{
    // Poll the stop flag in short slices so Stop() doesn't wait a full delay.
    constexpr int slice = 100;
    for (int waited = 0; waited < ms && !stopRequested_; waited += slice)
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
}

void SenderApp::RunLoop(std::string ip, uint16_t port)
{
    // One connection per iPad, machine-wide: a per-IP named mutex. A second
    // instance (CLI or tray) aimed at the same iPad backs off instead of
    // fighting over its single listening socket and the virtual display.
    // Different IPs get different names, so multiple iPads run fine.
    std::wstring lockName = L"Global\\opendisplay-win-";
    for (char c : ip)
        lockName += (c == '.' || c == ':') ? L'_' : static_cast<wchar_t>(c);
    HANDLE ipLock = CreateMutexW(nullptr, FALSE, lockName.c_str());
    if (ipLock != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        Logf(ip, "another opendisplay-win is already connected to this iPad\n");
        CloseHandle(ipLock);
        state_ = State::Idle;
        return;
    }

    // Declaration order matters for teardown: H264Encoder's ctor initializes
    // COM (MTA) + Media Foundation for this thread and its dtor uninitializes
    // them. Locals are destroyed in reverse, so the encoder is declared FIRST
    // (destroyed LAST) — otherwise DesktopDuplication's D3D11/DXGI COM objects
    // would be released after CoUninitialize(), an access violation on Stop().
    H264Encoder encoder;
    VirtualDisplay vdisp;
    DesktopDuplication dup;
    InputInjector input;
    std::mutex pipelineMutex;

    // Keeps this sender's monitor position separate from the other iPads' —
    // several senders share one HKCU key.
    vdisp.SetIdentity(ip);

    auto buildPipeline = [&](uint32_t width, uint32_t height) {
        // Caller holds pipelineMutex.
        if (!vdisp.IsOpen() && !vdisp.Open()) {
            Logf(ip, "VirtualDisplay::Open failed (parsec-vdd driver missing/inaccessible?)\n");
            return false;
        }
        // Release the capture BEFORE touching the monitor. On a rotation rebuild
        // EnsureResolution removes and re-adds the virtual display; doing that
        // while a DXGI duplication is still live on the old output tears the
        // monitor down under an active capture — which destabilizes DWM (the
        // Display Settings dialog crashes) and can crash us. Closing first means
        // no duplication ever references a monitor that's being replaced.
        dup.Close();
        if (!vdisp.EnsureResolution(width, height, kFps)) {
            Logf(ip, "VirtualDisplay::EnsureResolution failed (run as Administrator?)\n");
            return false;
        }
        if (!dup.Open(vdisp.DeviceName())) {
            Logf(ip, "DesktopDuplication::Open failed\n");
            return false;
        }
        if (!encoder.Configure(dup.Width(), dup.Height(), kFps, kBitrateBps)) {
            Logf(ip, "encoder configure failed\n");
            return false;
        }
        input.SetMonitorRect(vdisp.MonitorRect());
        Logf(ip, "pipeline ready: %ux%u\n", dup.Width(), dup.Height());
        return true;
    };

    // RAII: publish the current connection so Stop() can close it, and clear it
    // before the connection is destroyed (dtors run in reverse declaration
    // order, so declare this *after* the connection it points at).
    struct ActiveConn {
        SenderApp* self;
        ActiveConn(SenderApp* s, Connection* c) : self(s)
        {
            std::lock_guard<std::mutex> lock(s->connMutex_);
            s->activeConn_ = c;
        }
        ~ActiveConn()
        {
            std::lock_guard<std::mutex> lock(self->connMutex_);
            self->activeConn_ = nullptr;
        }
    };

    bool blockedLogged = false; // the wait message belongs in the log once, not every retry

    while (!stopRequested_) {
        // A blocked sender keeps checking back every few seconds; flipping it to
        // Connecting for each of those attempts would make the tray entry
        // alternate between "waiting for 2732x2048" and "connecting..." while
        // nothing about its situation changed.
        if (state_ != State::Blocked)
            state_ = State::Connecting;
        Logf(ip, "connecting to port %u ...\n", port);
        auto conn = Connection::Connect(ip, port);
        if (!conn) {
            Logf(ip, "connect failed, retrying in %dms\n", kReconnectDelayMs);
            InterruptibleSleep(kReconnectDelayMs);
            continue;
        }
        ActiveConn activeConn(this, &*conn);
        Logf(ip, "connected, waiting for hello...\n");

        HelloMsg hello;
        bool gotHello = false;
        while (!gotHello && !stopRequested_) {
            auto frame = conn->ReadFrame();
            if (!frame)
                break;
            if (!IsControlPayload(frame->data(), frame->size()))
                continue;
            auto msg = ParseControlMessage(frame->data(), frame->size());
            if (msg && msg->type == ControlType::Hello) {
                hello = msg->hello;
                gotHello = true;
            }
        }
        if (!gotHello)
            continue;

        // Version handshake (receiver protocol 3+): the iPad only sends
        // `pencil`/`proximity` to a peer that announced protocol >= 3 —
        // without this it silently degrades the Apple Pencil to plain `touch`
        // and pressure never arrives. Sent once per connection and only from
        // this thread: the reader thread must never write to the socket, or
        // its frames would interleave with the video the capture loop sends.
        static constexpr char kWelcome[] = "{\"type\":\"welcome\",\"pv\":3,\"min\":1}";
        bool welcomeSent = conn->SendFrame(reinterpret_cast<const uint8_t*>(kWelcome), sizeof(kWelcome) - 1);
        Logf(ip, "welcome sent: %s\n", welcomeSent ? "yes" : "FAILED");

        uint32_t width = EvenClamp(hello.pixelsWide, 1920);
        uint32_t height = EvenClamp(hello.pixelsHigh, 1080);
        Logf(ip, "hello: %dx%d -> %ux%u\n", hello.pixelsWide, hello.pixelsHigh, width, height);

        // Only iPads of the panel size that is already on the air may join (see
        // AcquirePanel). A different one hangs up and keeps checking back, so
        // it starts by itself once the other iPad disconnects.
        PanelHolder panel;
        uint32_t activeWidth = 0, activeHeight = 0;
        panel.held = AcquirePanel(width, height, activeWidth, activeHeight);
        if (!panel.held) {
            blockedByWidth_ = activeWidth;
            blockedByHeight_ = activeHeight;
            state_ = State::Blocked;
            // Once per episode, not on every check: this comes back every few
            // seconds for as long as the other iPad streams.
            if (!blockedLogged) {
                blockedLogged = true;
                Logf(ip, "waiting: an iPad with a %ux%u panel is streaming and this one is %ux%u — parsec-vdd only "
                         "holds one custom resolution at a time\n",
                     activeWidth, activeHeight, width, height);
            }
            conn->Close();
            InterruptibleSleep(kBlockedRetryMs);
            continue;
        }
        blockedByWidth_ = 0;
        blockedByHeight_ = 0;
        blockedLogged = false;

        bool pipelineOk;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex);
            pipelineOk = buildPipeline(width, height);
        }
        if (!pipelineOk) {
            conn->Close();
            InterruptibleSleep(kReconnectDelayMs);
            continue;
        }

        width_ = width;
        height_ = height;
        state_ = State::Streaming;

        std::atomic<bool> running{true};
        bool loggedPencil = false; // reader-thread only; one line per connection

        std::thread reader([&] {
            while (running && !stopRequested_) {
                auto frame = conn->ReadFrame();
                if (!frame) {
                    running = false;
                    break;
                }
                if (!IsControlPayload(frame->data(), frame->size()))
                    continue;
                auto msg = ParseControlMessage(frame->data(), frame->size());
                if (!msg)
                    continue;

                switch (msg->type) {
                    case ControlType::Kf:
                        Logf(ip, "kf requested by receiver\n");
                        encoder.RequestKeyFrame();
                        break;
                    case ControlType::Hello: {
                        // Rotation (or any panel-size change): rebuild the
                        // whole pipeline for the new dimensions. Everything
                        // that reads or writes `width`/`height` does so under
                        // pipelineMutex — the capture loop compares the
                        // monitor against them (see the panel-size watchdog).
                        std::lock_guard<std::mutex> lock(pipelineMutex);
                        uint32_t w = EvenClamp(msg->hello.pixelsWide, width);
                        uint32_t h = EvenClamp(msg->hello.pixelsHigh, height);
                        Logf(ip, "hello again: %dx%d -> rebuilding pipeline at %ux%u\n", msg->hello.pixelsWide,
                               msg->hello.pixelsHigh, w, h);
                        // Turning the iPad changes the size under the claim.
                        // Alone that's fine; with another iPad attached the two
                        // would be back to fighting over the one custom
                        // resolution the driver has, so say so and carry on —
                        // hanging up on the user for turning their iPad would
                        // be worse.
                        if (!RetunePanel(w, h))
                            Logf(ip, "rotating while another iPad is attached — its picture may end up "
                                     "letterboxed until one of you reconnects\n");

                        if (buildPipeline(w, h)) {
                            width = w;
                            height = h;
                            width_ = w;
                            height_ = h;
                        } else {
                            Logf(ip, "pipeline rebuild failed, disconnecting\n");
                            running = false;
                        }
                        break;
                    }
                    case ControlType::Touch:
                        // No pipelineMutex here on purpose: the capture loop
                        // holds it ~90% of the time (each capture blocks up to
                        // one frame interval), and taking it here starved
                        // input injection — the actual cause of the sluggish
                        // touch/drag. `input` is only ever touched by this
                        // reader thread (its initial SetMonitorRect happens on
                        // the main thread before this thread starts, and
                        // rotation rebuilds run on this same thread), so no
                        // lock is needed. Refresh the mapping rect first
                        // (thread-safe read) so a live monitor drag is followed
                        // immediately instead of only after the next reconnect.
                        input.SetMonitorRect(vdisp.MonitorRect());
                        input.HandleTouch(msg->touch);
                        break;
                    case ControlType::Scroll:
                        input.SetMonitorRect(vdisp.MonitorRect());
                        input.HandleScroll(msg->scroll);
                        break;
                    case ControlType::Pencil:
                        if (!loggedPencil) {
                            // Proof the version handshake landed: without our
                            // `welcome` the iPad would send `touch` here.
                            loggedPencil = true;
                            Logf(ip, "pencil input active (receiver honoured welcome pv=3)\n");
                        }
                        input.SetMonitorRect(vdisp.MonitorRect());
                        input.HandlePencil(msg->pencil);
                        break;
                    case ControlType::Proximity:
                        input.SetMonitorRect(vdisp.MonitorRect());
                        input.HandleProximity(msg->proximity);
                        break;
                    default:
                        break;
                }
            }
        });

        std::vector<uint8_t> nv12;
        auto lastSend = std::chrono::steady_clock::now();
        auto lastChange = std::chrono::steady_clock::now() - std::chrono::milliseconds(kActiveTailMs);
        // A drop is "pending" until something goes out again. Guards the replay
        // below against re-arming itself on every failed attempt.
        bool dropPending = false;
        // Set when the monitor rect couldn't be read right after a geometry
        // change (the desktop can still be mid-reconfigure); retried below
        // until it succeeds, because nothing else refreshes it in place.
        bool rectStale = false;

        // Watchdog for the panel size (see the check further down): when the
        // monitor is left on a size that isn't this iPad's, this is when it
        // started, and how many rebuilds we already spent on it.
        std::chrono::steady_clock::time_point wrongSizeSince{};
        bool sizeRebuildDone = false;
        bool sizeGiveUpLogged = false;

        while (running && !stopRequested_) {
            std::vector<EncodedFrame> encoded;
            {
                // Only capture+encode need the pipeline lock (they touch dup
                // and encoder, which the reader thread may rebuild on
                // rotation). The network send is deliberately outside the
                // lock so a slow link never stalls a pending rebuild. Input
                // injection deliberately does NOT take this lock (see the
                // reader thread) — this loop holds it almost continuously.
                std::lock_guard<std::mutex> lock(pipelineMutex);

                nv12.resize(static_cast<size_t>(dup.Width()) * dup.Height() * 3 / 2);
                // CaptureFrameNv12 returns false on a pure timeout (nothing on
                // screen or cursor changed since last time).
                bool changed = dup.CaptureFrameNv12(nv12, 1000 / static_cast<int>(kFps));

                // Rotation or a resolution change made on the Windows side
                // never sends a `hello`, so nothing rebuilds the pipeline: the
                // encoder would keep the old geometry and read the new frame
                // with the wrong stride — a skewed picture on the iPad while
                // the host's own screenshot looks fine. Follow the capture.
                //
                // Only once a frame actually arrived: the recovery path in
                // CaptureFrameNv12 reopens the duplication and takes fresh
                // dimensions from the ModeDesc while returning false, so
                // dup.Width()/Height() can already describe the new geometry
                // while nv12 still holds the previous frame. Reconfiguring on
                // that would encode the old buffer with the new stride — one
                // skewed frame, exactly what this is here to prevent.
                if (changed && (encoder.Width() != dup.Width() || encoder.Height() != dup.Height())) {
                    Logf(ip, "capture is now %ux%u (encoder had %ux%u), reconfiguring\n", dup.Width(), dup.Height(),
                           encoder.Width(), encoder.Height());
                    if (encoder.Configure(dup.Width(), dup.Height(), kFps, kBitrateBps)) {
                        // The cached rect is only refreshed when the monitor
                        // moves, and a rotation in place doesn't move it.
                        rectStale = !vdisp.QueryMonitorRect();
                        input.SetMonitorRect(vdisp.MonitorRect());
                        encoder.RequestKeyFrame();
                        width_ = dup.Width();
                        height_ = dup.Height();
                    } else {
                        Logf(ip, "encoder reconfigure failed, dropping the connection\n");
                        running = false;
                    }
                } else if (rectStale && vdisp.QueryMonitorRect()) {
                    // The desktop was still mid-reconfigure above. Without this
                    // retry the touch mapping would stay on the old geometry
                    // until the monitor is moved or the pipeline rebuilt.
                    input.SetMonitorRect(vdisp.MonitorRect());
                    rectStale = false;
                }

                auto now = std::chrono::steady_clock::now();

                // Adding or removing *any* parsec virtual display resets the
                // mode of *every* parsec monitor; Windows then restores each
                // one from what it last persisted for that display path. With
                // two iPads the paths get swapped around, so a neighbour
                // connecting can leave this monitor on the other iPad's size —
                // the picture then arrives letterboxed on this panel. The
                // reconfigure above keeps it correct but wrong-shaped, so once
                // the churn has settled, put our own size back.
                //
                // Only when the size is neither the panel's nor the panel
                // rotated: a rotation made in Windows is the user's decision
                // and is adopted, not undone.
                if (dup.Width() == height && dup.Height() == width) {
                    std::swap(width, height); // rotated in Windows: that is the panel size now
                }
                // Exactly one attempt, and only after the churn has settled. A
                // rebuild resets every parsec monitor in turn, so retrying is
                // how two senders end up trading rebuilds forever — and a
                // second attempt can't help anyway: either Windows had merely
                // restored a stale mode for this display path (the rebuild
                // fixes that), or another sender's panel size is in force, and
                // then no amount of rebuilding wins (see the note below).
                if (dup.Width() == width && dup.Height() == height) {
                    wrongSizeSince = {};
                    sizeRebuildDone = false;
                } else if (wrongSizeSince == std::chrono::steady_clock::time_point{}) {
                    wrongSizeSince = now;
                } else if (!sizeRebuildDone &&
                           now - wrongSizeSince > std::chrono::milliseconds(kWrongSizeGraceMs)) {
                    // A remove + re-add is what gets the panel size back;
                    // re-applying the mode on the live monitor is refused
                    // (DISP_CHANGE_BADMODE) while the capture runs.
                    sizeRebuildDone = true;
                    Logf(ip, "monitor sits at %ux%u instead of %ux%u, rebuilding once\n", dup.Width(), dup.Height(),
                         width, height);
                    if (!buildPipeline(width, height))
                        Logf(ip, "rebuild for the panel size failed, keeping what we have\n");
                    else
                        continue; // nv12 still holds the pre-rebuild frame — capture a fresh one
                } else if (sizeRebuildDone && !sizeGiveUpLogged) {
                    // parsec-vdd puts *one* custom resolution on all of its
                    // virtual monitors: whichever sender sets its panel size
                    // last drags every other monitor along. Two iPads with
                    // different panels therefore can't both run native, and
                    // the smaller one shows the picture letterboxed. Said once
                    // per connection, then we stop touching the monitor.
                    sizeGiveUpLogged = true;
                    Logf(ip, "monitor stays at %ux%u (this iPad is %ux%u): parsec-vdd shares one custom resolution "
                             "across all its monitors, so the picture stays letterboxed here\n",
                         dup.Width(), dup.Height(), width, height);
                }

                if (changed)
                    lastChange = now;

                // The async encoder holds one frame until the *next* frame is
                // fed, so if we stopped feeding the instant the screen went
                // idle, the last frame of an interaction (a tap, the end of a
                // scroll) would sit in the encoder until the next change or
                // keepalive — up to a second later, which feels sluggish. Keep
                // feeding at capture rate for a short tail after the last
                // change so the encoder stays drained and interaction stays
                // low-latency.
                bool active = now - lastChange < std::chrono::milliseconds(kActiveTailMs);

                // Keepalive re-encodes the last image (a tiny P-frame) purely
                // so the iPad's ~5s liveness watchdog (spec §5) never trips on
                // an otherwise idle desktop. Not gated on "captured a real
                // frame yet": if the first DXGI frame is delayed past the
                // watchdog, this still sends the (zero-filled) buffer so the
                // connection survives until real content arrives.
                bool keepaliveDue = now - lastSend >= std::chrono::milliseconds(kKeepaliveMs);

                if (running && (active || keepaliveDue))
                    encoded = encoder.EncodeNv12(nv12.data(), nv12.size());
            }

            bool sentSomething = false;
            for (auto& f : encoded) {
                // Backpressure: if the socket can't take data within the
                // budget, drop the whole frame (never a partial write — that
                // would desync the receiver's framing) and force a keyframe
                // so the next frame we do send resyncs the decoder.
                if (!conn->WaitWritable(kSendTimeoutMs)) {
                    encoder.RequestKeyFrame();
                    // A dropped frame leaves the receiver on the previous
                    // image, and Desktop Duplication delivers nothing new once
                    // the desktop goes static, so the correction would wait for
                    // the keepalive — up to a second. Count the drop as a
                    // change: the active tail re-feeds the last captured buffer
                    // right away, no second timer needed (upstream does the
                    // same with a dedicated 30ms replay timer, see #207).
                    //
                    // Only for the *first* drop of an episode. Re-arming on
                    // every failed attempt would keep `active` true for as long
                    // as the link stays congested, burning an encode per
                    // WaitWritable timeout on frames nobody can receive. One
                    // prompt replay, then fall back to the keepalive until the
                    // socket drains.
                    if (!dropPending) {
                        lastChange = std::chrono::steady_clock::now();
                        dropPending = true;
                        // Once per episode, not per dropped frame: on a link
                        // that stays congested this would otherwise be the
                        // loudest line in the log.
                        Logf(ip, "send backpressure, dropped a frame\n");
                    }
                    break;
                }
                if (!conn->SendFrame(f.annexB.data(), static_cast<uint32_t>(f.annexB.size()))) {
                    // A real send error (blocking send, so not a timeout):
                    // the connection is gone. Drop it and let the outer loop
                    // reconnect + resync with a fresh hello and keyframe.
                    running = false;
                    break;
                }
                sentSomething = true;
                dropPending = false;
                if (f.isKeyFrame)
                    Logf(ip, "sent keyframe, %zu bytes\n", f.annexB.size());
            }
            if (sentSomething)
                lastSend = std::chrono::steady_clock::now();
        }

        conn->Close();
        reader.join();
        // After the reader is gone, so nothing else touches the injector.
        input.EndSession();
        width_ = 0;
        height_ = 0;
        Logf(ip, "disconnected%s\n", stopRequested_ ? "" : ", reconnecting");
    }

    if (ipLock != nullptr)
        CloseHandle(ipLock);
    state_ = State::Idle;
}

} // namespace od
