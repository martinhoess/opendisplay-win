#include "app/SenderApp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

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
        fprintf(stderr, "another opendisplay-win is already connected to %s\n", ip.c_str());
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

    auto buildPipeline = [&](uint32_t width, uint32_t height) {
        // Caller holds pipelineMutex.
        if (!vdisp.IsOpen() && !vdisp.Open()) {
            fprintf(stderr, "VirtualDisplay::Open failed (parsec-vdd driver missing/inaccessible?)\n");
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
            fprintf(stderr, "VirtualDisplay::EnsureResolution failed (run as Administrator?)\n");
            return false;
        }
        if (!dup.Open(vdisp.DeviceName())) {
            fprintf(stderr, "DesktopDuplication::Open failed\n");
            return false;
        }
        if (!encoder.Configure(dup.Width(), dup.Height(), kFps, kBitrateBps)) {
            fprintf(stderr, "encoder configure failed\n");
            return false;
        }
        input.SetMonitorRect(vdisp.MonitorRect());
        printf("pipeline ready: %ux%u\n", dup.Width(), dup.Height());
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

    while (!stopRequested_) {
        state_ = State::Connecting;
        printf("connecting to %s:%u ...\n", ip.c_str(), port);
        auto conn = Connection::Connect(ip, port);
        if (!conn) {
            fprintf(stderr, "connect failed, retrying in %dms\n", kReconnectDelayMs);
            InterruptibleSleep(kReconnectDelayMs);
            continue;
        }
        ActiveConn activeConn(this, &*conn);
        printf("connected, waiting for hello...\n");

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
        printf("welcome sent: %s\n", welcomeSent ? "yes" : "FAILED");

        uint32_t width = EvenClamp(hello.pixelsWide, 1920);
        uint32_t height = EvenClamp(hello.pixelsHigh, 1080);
        printf("hello: %dx%d -> %ux%u\n", hello.pixelsWide, hello.pixelsHigh, width, height);

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
                        printf("kf requested by receiver\n");
                        encoder.RequestKeyFrame();
                        break;
                    case ControlType::Hello: {
                        // Rotation (or any panel-size change): rebuild the
                        // whole pipeline for the new dimensions.
                        uint32_t w = EvenClamp(msg->hello.pixelsWide, width);
                        uint32_t h = EvenClamp(msg->hello.pixelsHigh, height);
                        printf("hello again: %dx%d -> rebuilding pipeline at %ux%u\n", msg->hello.pixelsWide,
                               msg->hello.pixelsHigh, w, h);
                        std::lock_guard<std::mutex> lock(pipelineMutex);
                        if (buildPipeline(w, h)) {
                            width_ = w;
                            height_ = h;
                        } else {
                            fprintf(stderr, "pipeline rebuild failed, disconnecting\n");
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
                            printf("pencil input active (receiver honoured welcome pv=3)\n");
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

                auto now = std::chrono::steady_clock::now();
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

                if (active || keepaliveDue)
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
                        printf("send backpressure, dropped a frame\n");
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
                    printf("sent keyframe, %zu bytes\n", f.annexB.size());
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
        printf("disconnected%s\n", stopRequested_ ? "" : ", reconnecting");
    }

    if (ipLock != nullptr)
        CloseHandle(ipLock);
    state_ = State::Idle;
}

} // namespace od
