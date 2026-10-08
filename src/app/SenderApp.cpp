#include "app/SenderApp.h"

#include "app/Log.h"
#include "app/Config.h"
#include "app/EncodeProfile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <climits>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <winsock2.h>

#include "display/DesktopDuplication.h"

#include <wrl/client.h>
#include "display/ExistingMonitor.h"
#include "display/MttVddSettings.h"
#include "encode/H264Encoder.h"
#include "input/InputInjector.h"
#include "net/Connection.h"
#include "net/UsbMux.h"
#include "net/CursorMessages.h"
#include "net/CursorUdp.h"
#include "net/Protocol.h"

namespace od {

namespace {

constexpr uint32_t kFps = 60;
constexpr int kSendTimeoutMs = 500;      // backpressure: how long to wait for the socket before dropping a frame
constexpr int kDynamicHealthyMs = 5000;  // climb peak back toward ceiling after this quiet stretch
constexpr int kDynamicLogMinMs = 2000;   // sparse Dynamic peak-change logging
constexpr int kReconnectDelayMs = 2000;
constexpr int kKeepaliveMs = 1000;       // max silence on a static screen; well under the iPad's ~5s watchdog
constexpr int kActiveTailMs = 300;       // keep feeding the encoder this long after the last change (drains its 1-frame hold)
constexpr int kWrongSizeGraceMs = 3000;  // how long the monitor may sit on a foreign size before we rebuild it
constexpr int kBlockedRetryMs = 5000;    // how often a waiting iPad checks whether the display is free again
constexpr int kCursorUdpAckMs = 5000;    // drop UDP and stay on TCP if no cursorAck (PROTOCOL 6.3)

// Only one panel size may be on the air at a time.
//
// MTT VDD: modes are independent per monitor (merged into vdd_settings.xml),
// but we still serialize panel sizes so two senders never fight over rebuilds.
// Same-size iPads share; a different size waits until the display is free.
// (Parsec VDD path removed - this fork is MTT-only.)
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

// Returns true when this release dropped the holder count to zero (last client).
bool ReleasePanel()
{
    PanelState& panel = Panel();
    std::lock_guard<std::mutex> lock(panel.mutex);
    if (--panel.holders <= 0) {
        panel.holders = 0;
        panel.width = 0;
        panel.height = 0;
        return true;
    }
    return false;
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

uint32_t Align16Clamp(int32_t v, uint32_t fallback)
{
    uint32_t u = v > 0 ? static_cast<uint32_t>(v) : fallback;
    u &= ~15u; // H.264 / QSV want multiples of 16 (also keeps NV12 chroma even)
    if (u == 0)
        u = fallback & ~15u;
    return u;
}

// Pad NV12 up to encoder size (16-ceil of capture). Floor-crop (1080->1072)
// made every frame look like a size change and reconfigure-flashed black.
bool PadNv12TopLeft(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
                    uint32_t dstW, uint32_t dstH, std::vector<uint8_t>& dst)
{
    if (dstW == 0 || dstH == 0 || dstW < srcW || dstH < srcH)
        return false;
    if (srcW == dstW && srcH == dstH) {
        dst = src;
        return true;
    }
    dst.assign(dstW * dstH * 3 / 2, 0);
    std::fill(dst.begin() + static_cast<std::ptrdiff_t>(dstW * dstH), dst.end(),
              static_cast<uint8_t>(0x80));
    for (uint32_t y = 0; y < srcH; ++y)
        memcpy(dst.data() + y * dstW, src.data() + y * srcW, srcW);
    const uint8_t* srcUv = src.data() + srcW * srcH;
    uint8_t* dstUv = dst.data() + dstW * dstH;
    for (uint32_t y = 0; y < srcH / 2; ++y)
        memcpy(dstUv + y * dstW, srcUv + y * srcW, srcW);
    return true;
}

uint32_t Align16Ceil(uint32_t v)
{
    return (v + 15u) & ~15u;
}

int64_t UnixEpochMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// PROTOCOL 5.1 telemetry prefix - must precede the first 00 00 00 01 start code.
std::vector<uint8_t> PrefixAnnexBWithTiming(const std::vector<uint8_t>& annexB, int64_t capMs, int64_t sndMs)
{
    char head[80];
    const int n = snprintf(head, sizeof(head), "{\"cap\":%lld,\"snd\":%lld}",
                           static_cast<long long>(capMs), static_cast<long long>(sndMs));
    if (n <= 0)
        return annexB;
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(n) + annexB.size());
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(head),
               reinterpret_cast<const uint8_t*>(head) + n);
    out.insert(out.end(), annexB.begin(), annexB.end());
    return out;
}

std::string MakePongMessage(double tEcho, int64_t mtMs)
{
    char buf[128];
    // Echo t as a JSON number; mt is sender wall-clock ms (PROTOCOL 6.2 / 8.1).
    snprintf(buf, sizeof(buf), "{\"type\":\"pong\",\"t\":%.0f,\"mt\":%lld}",
             tEcho, static_cast<long long>(mtMs));
    return std::string(buf);
}

std::string MakeSenderPingMessage()
{
    return std::string("{\"type\":\"ping\"}");
}



// usbmuxd cannot carry UDP (PROTOCOL 6.3). Loopback dials are the USB
// binding on Windows when we add it; ignore cursorPort there.
bool IsUsbLikeHost(const std::string& host)
{
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
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
    transport_ = Transport::None;
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
    DesktopDuplication dup;
    InputInjector input;
    std::mutex pipelineMutex;
    bool gpuNv12Path = false; // VideoProcessor BGRA->NV12 + EncodeDxgiNv12
    bool haveGpuDesktopFrame = false; // true after first successful GPU present

    // MTT-only: capture + QSV share the Intel VDD. No Parsec fallback.
    bool usingMttVdd = false;
    bool mttModesEnsured = false; // once per connection; Ensure is idempotent too
    // Raw hello pixelsWide/High (unaligned): the per-iPad EDID is generated
    // from the panel size the iPad reports, not the 16-aligned capture size.
    uint32_t mttHelloW = 0, mttHelloH = 0;
    bool mttHelloModeForced = false; // force hello WxH once; later honor Keep
    ExistingMonitor mttMon{};
    // Updated once the dial settles (USB vs Wi-Fi); buildPipeline / reconfigure read these.
    EncodeKnobs sessionEncode;
    EncodeMode sessionEncodeMode = EncodeMode::Dynamic;
    EncodePreset sessionEncodePreset = EncodePreset::Speed;

    auto buildPipeline = [&](uint32_t width, uint32_t height) {
        // Caller holds pipelineMutex.
        dup.Close();

        // Per-iPad EDID BEFORE the head attaches: modes generated from this
        // hello size (first build of a connection = hello WxH). Same iPad
        // again = no-op; a size change self-elevates once (tray notice first)
        // and reloads MttVDD. UAC decline keeps the installed EDID and still
        // connects. Never re-run on capture rebuilds.
        if (!mttModesEnsured) {
            const uint32_t edidW = mttHelloW ? mttHelloW : width;
            const uint32_t edidH = mttHelloH ? mttHelloH : height;
            (void)EnsureMttResolutionsForHello(edidW, edidH, ip);
            mttModesEnsured = true; // even on UAC decline: one prompt per connection
        }

        // Re-attach MTT head if a prior last-client teardown dropped it.
        Logf(ip, "mtt: ensuring VDD for hello\n");
        (void)EnsureMttVddAttached(ip);
        usingMttVdd = FindMttVirtualMonitor(mttMon);
        if (usingMttVdd) {
            Logf(ip, "using MTT virtual monitor %ls (%ls)\n",
                 mttMon.deviceName.c_str(), mttMon.deviceString.c_str());
            // First hello of a connection: restore the saved mode (the user's
            // Display Settings pick) only if this iPad's EDID offers it;
            // otherwise the preferred mode. Later rebuilds honor Keep and only
            // fix orientation / refresh.
            if (!mttHelloModeForced) {
                uint32_t savedW = 0, savedH = 0, savedHz = 0;
                if (QueryMttSavedMode(savedW, savedH, savedHz)) {
                    const uint32_t applyHz = savedHz ? savedHz : kFps;
                    if (!EnsureMonitorMode(mttMon.deviceName, savedW, savedH, applyHz)) {
                        Logf(ip, "mtt: restoring saved mode %ux%u@%u failed (keeping current)\n",
                             savedW, savedH, applyHz);
                        if (EnsureMonitorLandscapeOrientation(mttMon.deviceName))
                            Logf(ip, "MTT orientation ensured landscape (resolution unchanged)\n");
                        if (!EnsureMonitorRefresh(mttMon.deviceName, applyHz))
                            Logf(ip, "MTT refresh %u Hz request failed (keeping current)\n", applyHz);
                    } else {
                        Logf(ip, "mtt: restoring saved mode %ux%u@%u\n", savedW, savedH, applyHz);
                    }
                } else {
                    // No saved mode, or it is not offered by this iPad's EDID:
                    // use the preferred (landscape hello-native) mode.
                    const MttMode preferred = CurrentMttModes().front();
                    if (!EnsureMonitorMode(mttMon.deviceName, preferred.width, preferred.height, kFps)) {
                        Logf(ip, "mtt: no usable saved mode - preferred %ux%u failed "
                                 "(keeping current)\n",
                             preferred.width, preferred.height);
                        if (!EnsureMonitorRefresh(mttMon.deviceName, kFps))
                            Logf(ip, "MTT refresh %u Hz request failed (keeping current)\n", kFps);
                        else
                            Logf(ip, "MTT refresh set to %u Hz\n", kFps);
                    } else {
                        Logf(ip, "mtt: no usable saved mode - using preferred %ux%u\n", preferred.width,
                             preferred.height);
                    }
                }
                mttHelloModeForced = true;
            } else {
                if (EnsureMonitorLandscapeOrientation(mttMon.deviceName))
                    Logf(ip, "MTT orientation ensured landscape (resolution unchanged)\n");
                else
                    Logf(ip, "MTT landscape orientation request failed (keeping current)\n");
                if (!EnsureMonitorRefresh(mttMon.deviceName, kFps))
                    Logf(ip, "MTT refresh %u Hz request failed (keeping current)\n", kFps);
            }
            if (!dup.Open(mttMon.deviceName)) {
                Logf(ip, "DesktopDuplication::Open failed on MTT device\n");
                return false;
            }
            if (!GetMonitorRectByDeviceName(mttMon.deviceName, mttMon.rect)) {
                // keep prior mttMon.rect if Enum failed
            }
            input.SetMonitorRect(mttMon.rect);
            Logf(ip, "mtt: input rect %ls L=%ld T=%ld R=%ld B=%ld\n", mttMon.deviceName.c_str(),
                 mttMon.rect.left, mttMon.rect.top, mttMon.rect.right, mttMon.rect.bottom);
        } else {
            Logf(ip, "MTT virtual monitor not found (Intel VDD required; Parsec path removed)\n");
            return false;
        }
        // Bind encoder here (not later on size-flip with HW disabled): QSV needs
        // an Intel D3D device + SET_D3D_MANAGER before the first Configure.
        // Prefer the capture device when it is Intel (MTT) so VideoProcessor
        // NV12 and QSV share one D3D11 device — no CPU BGRA->NV12 bounce.
        // Encode to the size the caller requested (capture WxH that triggered
        // rebuild). Open() ModeDesc often stays hello landscape while DXGI
        // frames are portrait after rotate - trusting ModeDesc flash-loops.
        uint32_t encW = Align16Ceil(width);
        uint32_t encH = Align16Ceil(height);
        if (!encW || !encH) {
            encW = Align16Ceil(dup.Width() ? dup.Width() : 1920);
            encH = Align16Ceil(dup.Height() ? dup.Height() : 1080);
        } else if (dup.Width() && dup.Height() &&
                   (Align16Ceil(dup.Width()) != encW || Align16Ceil(dup.Height()) != encH)) {
            Logf(ip, "Open ModeDesc %ux%u != requested encode %ux%u - using requested\n",
                 dup.Width(), dup.Height(), encW, encH);
        }
        gpuNv12Path = false;
        encoder.SetAllowHardware(true);
        {
            bool haveIntelD3d = false;
            if (dup.Device() && dup.Context() && encoder.AdoptD3DDevice(dup.Device(), dup.Context()))
                haveIntelD3d = true;
            else if (encoder.EnsureIntelEncoderDevice())
                haveIntelD3d = true;
            if (!haveIntelD3d)
                Logf(ip, "Intel encoder D3D device unavailable - HW encode may fall back\n");
        }
        if (!encoder.Configure(encW, encH, kFps, sessionEncode.peakBitrateBps,
                               sessionEncode.qualityVsSpeed, sessionEncode.gopSeconds,
                               sessionEncode.useQualityRc, sessionEncode.rcQuality)) {
            Logf(ip, "encoder Configure failed (%ux%u)\n", encW, encH);
            return false;
        }
        // VBR only: Configure seeds mean ~60%; push preset mean (Speed/Balanced).
        if (!sessionEncode.useQualityRc)
            encoder.UpdateBitrate(sessionEncode.peakBitrateBps, sessionEncode.meanBitrateBps);
        if (encoder.UsesDxgiInput() && encoder.D3DDevice() && encoder.D3DDevice() == dup.Device() &&
            dup.EnsureGpuNv12Converter(encoder.Width(), encoder.Height())) {
            gpuNv12Path = true;
            Logf(ip, "GPU NV12 path active (VideoProcessor BGRA->NV12 on capture/QSV device)\n");
            haveGpuDesktopFrame = false;
        } else if (encoder.UsesDxgiInput()) {
            Logf(ip, "DXGI NV12 encode ready; GPU convert unbound - CPU BGRA->NV12 fallback\n");
        }
        {
            const std::wstring mftW = encoder.MftName();
            std::string mft;
            mft.reserve(mftW.size());
            for (wchar_t c : mftW)
                mft.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '?');
            if (mft.empty())
                mft = "(unknown)";
            if (sessionEncode.useQualityRc) {
                Logf(ip, "pipeline ready: %ux%u @ %u fps / Quality RC q=%u QVs=%u GOP=%us (soft max %u kbps), MFT: %s\n",
                     encoder.Width(), encoder.Height(), kFps, sessionEncode.rcQuality,
                     sessionEncode.qualityVsSpeed, sessionEncode.gopSeconds,
                     sessionEncode.peakBitrateBps / 1000, mft.c_str());
            } else {
                Logf(ip, "pipeline ready: %ux%u @ %u fps / VBR peak %u kbps mean %u kbps QVs=%u GOP=%us, MFT: %s\n",
                     encoder.Width(), encoder.Height(), kFps, sessionEncode.peakBitrateBps / 1000,
                     sessionEncode.meanBitrateBps / 1000, sessionEncode.qualityVsSpeed, sessionEncode.gopSeconds,
                     mft.c_str());
            }
        }
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

        bool stickToUsbAfterSession = false;

while (!stopRequested_) {
        // A blocked sender keeps checking back every few seconds; flipping it to
        // Connecting for each of those attempts would make the tray entry
        // alternate between "waiting for 2732x2048" and "connecting..." while
        // nothing about its situation changed.
        if (state_ != State::Blocked)
            state_ = State::Connecting;

        // USB preferred while usbmux + a USB device are available.
        // usbWifiFailover (default ON) keeps today's behavior: if USB dial
        // fails or the cable drops after a USB session, fall back to Wi-Fi.
        // When OFF, after a USB session we wait for USB again instead of
        // staying on Wi-Fi; Wi-Fi-only iPads (never USB this run) still work.
        const Config connectCfg = Config::Load();
        const bool allowWifiFailover = connectCfg.usbWifiFailover;

        bool usbSession = false;
        std::optional<Connection> conn;
        UsbMuxDevice usbDev{};
        const bool configuredLoopback = IsUsbLikeHost(ip);
        const bool usbAvailable = !configuredLoopback && ProbeUsbMux(usbDev) && usbDev.present;
        if (usbAvailable) {
            Logf(ip, "USB usbmux device %s, preferring 127.0.0.1:%u over Wi-Fi %s:%u\n",
                 usbDev.serial.empty() ? "?" : usbDev.serial.c_str(), kUsbMuxLocalPort,
                 ip.c_str(), port);
            if (EnsureUsbMuxForward(port, kUsbMuxLocalPort))
                conn = Connection::Connect("127.0.0.1", kUsbMuxLocalPort);
            if (!conn) {
                auto sock = ConnectUsbMux(usbDev.deviceId, port);
                if (sock)
                    conn = Connection::FromSocket(*sock);
            }
            if (conn) {
                usbSession = true;
                Logf(ip, "connected via USB (usbmux localhost:%u -> iPad:%u)\n",
                     kUsbMuxLocalPort, port);
            } else if (!allowWifiFailover) {
                Logf(ip, "USB usbmux dial failed, Wi-Fi failover off, retrying USB\n");
                InterruptibleSleep(kReconnectDelayMs);
                continue;
            } else {
                Logf(ip, "USB usbmux dial failed, falling back to Wi-Fi %s:%u\n",
                     ip.c_str(), port);
            }
        }
        if (!conn) {
            if (!allowWifiFailover && stickToUsbAfterSession) {
                Logf(ip, "USB cable dropped, Wi-Fi failover off, waiting for USB\n");
                InterruptibleSleep(kReconnectDelayMs);
                continue;
            }
            Logf(ip, "connecting to %s:%u ...\n", ip.c_str(), port);
            conn = Connection::Connect(ip, port);
        }
        if (!conn) {
            Logf(ip, "connect failed, retrying in %dms\n", kReconnectDelayMs);
            InterruptibleSleep(kReconnectDelayMs);
            continue;
        }
        ActiveConn activeConn(this, &*conn);
        transport_ = usbSession ? Transport::Usb : Transport::Wifi;
        if (usbSession) {
            // Remember a successful USB session so a later cable-drop does not
            // silently move to Wi-Fi when the user turned failover off.
            stickToUsbAfterSession = !allowWifiFailover;
            Logf(ip, "USB path waiting for hello...\n");
        } else {
            Logf(ip, "connected via Wi-Fi, waiting for hello...\n");
        }

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

        // Learn USB serial <-> hello.id so Settings can merge Nearby without guessing.
        if (usbSession && !usbDev.serial.empty() && !hello.id.empty()) {
            Config learned = Config::Load();
            learned.RememberUsbSerialId(usbDev.serial, hello.id);
            learned.Save();
            Logf(ip, "USB serial %s bound to id %s\n", usbDev.serial.c_str(), hello.id.c_str());
        }

        // Version handshake (receiver protocol 3+): the iPad only sends
        // `pencil`/`proximity` to a peer that announced protocol >= 3 —
        // without this it silently degrades the Apple Pencil to plain `touch`
        // and pressure never arrives. Sent once per connection and only from
        // this thread: the reader thread must never write to the socket, or
        // its frames would interleave with the video the capture loop sends.
        static constexpr char kWelcome[] = "{\"type\":\"welcome\",\"pv\":3,\"min\":1}";
        bool welcomeSent = conn->SendFrame(reinterpret_cast<const uint8_t*>(kWelcome), sizeof(kWelcome) - 1);
        Logf(ip, "welcome sent: %s\n", welcomeSent ? "yes" : "FAILED");

        uint32_t width = Align16Clamp(hello.pixelsWide, 1920);
        uint32_t height = Align16Clamp(hello.pixelsHigh, 1080);
        Logf(ip, "hello: %dx%d -> %ux%u\n", hello.pixelsWide, hello.pixelsHigh, width, height);
        mttHelloW = hello.pixelsWide > 0 ? static_cast<uint32_t>(hello.pixelsWide) : 0;
        mttHelloH = hello.pixelsHigh > 0 ? static_cast<uint32_t>(hello.pixelsHigh) : 0;

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
                Logf(ip, "waiting: an iPad with a %ux%u panel is streaming and this one is %ux%u - only one "
                         "panel size at a time (MTT keeps sizes serialized)\n",
                     activeWidth, activeHeight, width, height);
            }
            conn->Close();
            InterruptibleSleep(kBlockedRetryMs);
            continue;
        }
        blockedByWidth_ = 0;
        blockedByHeight_ = 0;
        blockedLogged = false;

        {
            // Reload prefs each connect so Settings changes apply without a full restart.
            const Config cfg = Config::Load();
            sessionEncodeMode = ParseEncodeMode(usbSession ? cfg.usbEncodeMode : cfg.wifiEncodeMode);
            sessionEncodePreset = ParseEncodePreset(usbSession ? cfg.usbEncodePreset : cfg.wifiEncodePreset);
            sessionEncode = ResolveEncodeKnobs(usbSession, sessionEncodeMode, sessionEncodePreset);
            if (sessionEncode.useQualityRc) {
                Logf(ip, "%s encode profile: mode=%s preset=%s QualityRC q=%u QVs=%u GOP=%us\n",
                     usbSession ? "USB" : "Wi-Fi",
                     EncodeModeName(sessionEncodeMode), EncodePresetName(sessionEncodePreset),
                     sessionEncode.rcQuality, sessionEncode.qualityVsSpeed, sessionEncode.gopSeconds);
            } else {
                Logf(ip, "%s encode profile: mode=%s preset=%s peak=%u kbps QVs=%u GOP=%us\n",
                     usbSession ? "USB" : "Wi-Fi",
                     EncodeModeName(sessionEncodeMode), EncodePresetName(sessionEncodePreset),
                     sessionEncode.peakBitrateBps / 1000, sessionEncode.qualityVsSpeed,
                     sessionEncode.gopSeconds);
            }
        }

        bool pipelineOk;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex);
            pipelineOk = buildPipeline(width, height);
        }
        if (!pipelineOk) {
            dup.Close();
            conn->Close();
            if (panel.held) {
                panel.held = false;
                if (ReleasePanel() && usingMttVdd) {
                    // Pipeline failed but session may reconnect - keep VDD sticky.
                    RequestMttVddTeardown(ip, MttTeardownReason::LinkLoss);
                    mttModesEnsured = false;
                    mttHelloModeForced = false;
                }
            }
            InterruptibleSleep(kReconnectDelayMs);
            continue;
        }

        width_ = width;
        height_ = height;
        state_ = State::Streaming;
        if (usingMttVdd)
            (void)SaveMttDisplayTopology(ip);

        std::atomic<bool> running{true};
        bool loggedPencil = false; // reader-thread only; one line per connection

        // UDP cursor side channel (PROTOCOL 6.3). Declared before the reader so
        // cursorAck can mark the channel confirmed. Positions get a session seq;
        // mirror on TCP until ack, then UDP-only. cursorImg stays TCP.
        CursorUdp cursorUdp;
        std::mutex cursorNetMutex;
        std::atomic<bool> cursorUdpConfirmed{false};
        uint64_t cursorSeq = 0;
        std::chrono::steady_clock::time_point cursorUdpOpened{};
        bool cursorUdpGaveUp = false;

                // Receiver ping → pong (PROTOCOL 8.1). Reader must not write the
        // socket (interleaves with video); queue t values for the capture loop.
        std::mutex pongMutex;
        std::vector<double> pendingPongTs;
        auto lastSenderPing = std::chrono::steady_clock::now();
        constexpr int kSenderPingMs = 2000;

        // Touch/pen/scroll must target the MTT capture head (mttMon).
        RECT lastLoggedMttInputRect{};
        bool loggedMttInputRect = false;
        auto refreshInputRect = [&]() {
            RECT r = mttMon.rect;
            if (GetMonitorRectByDeviceName(mttMon.deviceName, r))
                mttMon.rect = r;
            else
                r = mttMon.rect;
            input.SetMonitorRect(r);
            if (!loggedMttInputRect || r.left != lastLoggedMttInputRect.left ||
                r.top != lastLoggedMttInputRect.top || r.right != lastLoggedMttInputRect.right ||
                r.bottom != lastLoggedMttInputRect.bottom) {
                Logf(ip, "mtt: input rect %ls L=%ld T=%ld R=%ld B=%ld\n", mttMon.deviceName.c_str(),
                     r.left, r.top, r.right, r.bottom);
                lastLoggedMttInputRect = r;
                loggedMttInputRect = true;
            }
        };

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
                    case ControlType::Ping: {
                        std::lock_guard<std::mutex> plock(pongMutex);
                        pendingPongTs.push_back(msg->ping.t);
                        break;
                    }
                    case ControlType::Hello: {
                        // Rotation / panel-size change: rebuild. Same-size hello
                        // again (common right after connect) must not tear down
                        // capture+encoder under live input - that flash+crash.
                        std::lock_guard<std::mutex> lock(pipelineMutex);
                        uint32_t w = Align16Clamp(msg->hello.pixelsWide, width);
                        uint32_t h = Align16Clamp(msg->hello.pixelsHigh, height);
                        const int port = msg->hello.cursorPort;
                        auto retuneCursor = [&]() {
                            std::lock_guard<std::mutex> clock(cursorNetMutex);
                            if (port > 0 && port <= 65535 && !(usbSession || IsUsbLikeHost(ip))) {
                                if (!cursorUdp.Ready()) {
                                    if (cursorUdp.Open(ip, static_cast<uint16_t>(port))) {
                                        cursorUdpConfirmed = false;
                                        cursorUdpGaveUp = false;
                                        cursorUdpOpened = std::chrono::steady_clock::now();
                                        Logf(ip, "cursor UDP re-opened on port %d after hello\n", port);
                                    }
                                }
                            } else if (port <= 0) {
                                cursorUdp.Close();
                                cursorUdpConfirmed = false;
                            }
                        };
                        if (w == width && h == height) {
                            Logf(ip, "hello again: %dx%d -> size unchanged, keeping pipeline\n",
                                 msg->hello.pixelsWide, msg->hello.pixelsHigh);
                            retuneCursor();
                            break;
                        }
                        Logf(ip, "hello again: %dx%d -> rebuilding pipeline at %ux%u\n", msg->hello.pixelsWide,
                               msg->hello.pixelsHigh, w, h);
                        // Turning the iPad changes the size under the claim.
                        // Alone that's fine; with another iPad attached the two
                        // would be back to fighting over the one custom
                        // resolution the driver has, so say so and carry on -
                        // hanging up on the user for turning their iPad would
                        // be worse.
                        if (!RetunePanel(w, h))
                            Logf(ip, "rotating while another iPad is attached - its picture may end up "
                                     "letterboxed until one of you reconnects\n");

                        if (buildPipeline(w, h)) {
                            width = w;
                            height = h;
                            width_ = w;
                            height_ = h;
                            retuneCursor();
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
                        refreshInputRect();
                        input.HandleTouch(msg->touch);
                        break;
                    case ControlType::Scroll:
                        refreshInputRect();
                        input.HandleScroll(msg->scroll);
                        break;
                    case ControlType::Pencil:
                        if (!loggedPencil) {
                            // Proof the version handshake landed: without our
                            // `welcome` the iPad would send `touch` here.
                            loggedPencil = true;
                            Logf(ip, "pencil input active (receiver honoured welcome pv=3)\n");
                        }
                        refreshInputRect();
                        input.HandlePencil(msg->pencil);
                        break;
                    case ControlType::Proximity:
                        refreshInputRect();
                        input.HandleProximity(msg->proximity);
                        break;
                    case ControlType::CursorAck: {
                        std::lock_guard<std::mutex> clock(cursorNetMutex);
                        if (cursorUdp.Ready() && !cursorUdpConfirmed.exchange(true)) {
                            Logf(ip, "cursor UDP confirmed by receiver (positions leave TCP)\n");
                        }
                        break;
                    }
                    case ControlType::Stats:
                        // Free-form receiver telemetry (~5s). Prefix matches the
                        // Mac sender so one log file tells the whole story.
                        Logf(ip, "PHONE-STATS %.*s\n", static_cast<int>(frame->size()),
                             reinterpret_cast<const char*>(frame->data()));
                        break;
                    default:
                        break;
                }
            }
        });

        std::vector<uint8_t> nv12;
        std::string pendingCursorImg;
        std::string pendingCursor;
        auto lastSend = std::chrono::steady_clock::now();
        auto lastChange = std::chrono::steady_clock::now() - std::chrono::milliseconds(kActiveTailMs);
        // A drop is "pending" until something goes out again. Guards the replay
        // below against re-arming itself on every failed attempt.
        bool dropPending = false;
        auto lastBackpressure = std::chrono::steady_clock::now() - std::chrono::hours(1);
        auto lastDynamicAdjust = std::chrono::steady_clock::now() - std::chrono::hours(1);
        auto lastDynamicLog = std::chrono::steady_clock::now() - std::chrono::hours(1);
        // Set when the monitor rect couldn't be read right after a geometry
        // change (the desktop can still be mid-reconfigure); retried below
        // until it succeeds, because nothing else refreshes it in place.

        // Watchdog for the panel size (see the check further down): when the
        // monitor is left on a size that isn't this iPad's, this is when it
        // started, and how many rebuilds we already spent on it.

        // WiFi only: dial hello.cursorPort on the same host as TCP.
        std::string cursorProbe;
        if (hello.cursorPort > 0 && hello.cursorPort <= 65535 && !(usbSession || IsUsbLikeHost(ip))) {
            std::lock_guard<std::mutex> lock(cursorNetMutex);
            if (cursorUdp.Open(ip, static_cast<uint16_t>(hello.cursorPort))) {
                cursorUdpOpened = std::chrono::steady_clock::now();
                Logf(ip, "cursor UDP channel opened on %s:%d (mirroring TCP until cursorAck)\n",
                     ip.c_str(), hello.cursorPort);
                // Probe immediately so cursorAck does not need a mouse move
                // (PROTOCOL 6.3; Mac sender probes on UDP .ready).
                ++cursorSeq;
                cursorProbe = MakeCursorMessage(false, 0.0, 0.0, cursorSeq);
                cursorUdp.Send(reinterpret_cast<const uint8_t*>(cursorProbe.data()),
                               cursorProbe.size());
            } else {
                Logf(ip, "cursor UDP open failed for port %d, staying on TCP\n", hello.cursorPort);
            }
        } else if (hello.cursorPort > 0 && (usbSession || IsUsbLikeHost(ip))) {
            Logf(ip, "ignoring cursorPort=%d on USB-like host\n", hello.cursorPort);
        }
        if (!cursorProbe.empty()) {
            // Mirror the probe on TCP until ack (same seq; receiver dedups).
            if (!conn->SendFrame(reinterpret_cast<const uint8_t*>(cursorProbe.data()),
                                 static_cast<uint32_t>(cursorProbe.size()))) {
                running = false;
            }
        }

        std::chrono::steady_clock::time_point wrongSizeSince{};
        bool sizeRebuildDone = false;
        bool sizeGiveUpLogged = false;
        POINT mttLastPos{INT_MIN, INT_MIN};
        std::chrono::steady_clock::time_point lastMttPosPoll{};
        std::chrono::steady_clock::time_point waitFirstFrameSince{};
        bool firstFrameNudged = false;
        bool firstFrameWaitLogged = false;
        std::chrono::steady_clock::time_point lastEncSizeRebuild{};
        std::chrono::steady_clock::time_point lastPadFailLog{};
        // Local cadence: presents / encode-calls / AUs / sends per window.
        std::chrono::steady_clock::time_point cadenceSince = std::chrono::steady_clock::now();
        uint32_t cadencePresents = 0;
        uint32_t cadenceEncCalls = 0;
        uint32_t cadenceAus = 0;
        uint32_t cadenceSends = 0;
        uint32_t cadenceTimeouts = 0;

        while (running && !stopRequested_) {
            std::vector<EncodedFrame> encoded;
            int64_t capMs = 0;
            {
                // Only capture+encode need the pipeline lock (they touch dup
                // and encoder, which the reader thread may rebuild on
                // rotation). The network send is deliberately outside the
                // lock so a slow link never stalls a pending rebuild. Input
                // injection deliberately does NOT take this lock (see the
                // reader thread) — this loop holds it almost continuously.
                std::lock_guard<std::mutex> lock(pipelineMutex);

                // CaptureResult: desktopChanged drives encode; cursor*
                // go out as separate protocol messages (not baked into H.264).
                capMs = UnixEpochMs();
                Microsoft::WRL::ComPtr<ID3D11Texture2D> gpuNv12;
                CaptureResult cap{};
                if (gpuNv12Path) {
                    cap = dup.CaptureFrameNv12Gpu(gpuNv12, encoder.Width(), encoder.Height(),
                                                  1000 / static_cast<int>(kFps));
                    if (cap.desktopChanged && !gpuNv12) {
                        // VP convert failed mid-stream - drop to CPU for this process.
                        Logf(ip, "GPU NV12 capture failed - falling back to CPU BGRA->NV12\n");
                        gpuNv12Path = false;
                        haveGpuDesktopFrame = false;
                        nv12.resize(static_cast<size_t>(dup.Width()) * dup.Height() * 3 / 2);
                        cap = dup.CaptureFrameNv12(nv12, 1000 / static_cast<int>(kFps));
                    } else if (cap.desktopChanged && gpuNv12 && !haveGpuDesktopFrame) {
                        // First real present after connect/rebuild: IDR so the iPad
                        // leaves its black decoder state immediately.
                        haveGpuDesktopFrame = true;
                        waitFirstFrameSince = {};
                        firstFrameNudged = false;
                        firstFrameWaitLogged = false;
                        encoder.RequestKeyFrame();
                        Logf(ip, "first GPU desktop frame - keyframe requested\n");
                    }
                } else {
                    nv12.resize(static_cast<size_t>(dup.Width()) * dup.Height() * 3 / 2);
                    cap = dup.CaptureFrameNv12(nv12, 1000 / static_cast<int>(kFps));
                }
                const bool changed = cap.desktopChanged;
                if (changed)
                    ++cadencePresents;
                else if (!cap.acquired)
                    ++cadenceTimeouts;

                {
                    const auto nowCad = std::chrono::steady_clock::now();
                    if (nowCad - cadenceSince >= std::chrono::milliseconds(2000)) {
                        const double sec = std::chrono::duration<double>(nowCad - cadenceSince).count();
                        if (sec > 0.1) {
                            Logf(ip,
                                 "cadence: present=%.1f/s enc=%.1f/s au=%.1f/s send=%.1f/s timeout=%.1f/s\n",
                                 cadencePresents / sec, cadenceEncCalls / sec, cadenceAus / sec,
                                 cadenceSends / sec, cadenceTimeouts / sec);
                        }
                        cadenceSince = nowCad;
                        cadencePresents = cadenceEncCalls = cadenceAus = cadenceSends = cadenceTimeouts = 0;
                    }
                }

                // Mode change / ACCESS_LOST: CaptureFrame already Reopen()'d.
                // QSV DXGI path shares the capture D3D device with the MFT, so a
                // new capture device requires a full encoder rebuild.
                if (cap.accessLost) {
                    haveGpuDesktopFrame = false;
                    waitFirstFrameSince = {};
                    firstFrameNudged = false;
                    firstFrameWaitLogged = false;
                    Logf(ip, "capture ACCESS_LOST - rebuilding pipeline (new D3D device)\n");
                    gpuNv12Path = false;
                    // ACCESS_LOST is usually a Display Settings Apply. Persist the
                    // user's new layout first (only if MTT is attached+active and
                    // this was not our own apply); the rebuild never re-applies x/y.
                    if (usingMttVdd)
                        (void)SaveMttDisplayTopologyIfUserChange(ip);
                    if (!buildPipeline(width, height)) {
                        Logf(ip, "pipeline rebuild after ACCESS_LOST failed, dropping the connection\n");
                        running = false;
                    }
                    continue;
                }

                // MTT/VDD often never presents after DuplicateOutput until
                // something dirties the output - without that we sit on black
                // (no first GPU frame / no keepalive texture).
                if (gpuNv12Path && !haveGpuDesktopFrame) {
                    const auto nowWait = std::chrono::steady_clock::now();
                    if (waitFirstFrameSince == std::chrono::steady_clock::time_point{})
                        waitFirstFrameSince = nowWait;
                    const auto waited = nowWait - waitFirstFrameSince;
                    if (!firstFrameNudged &&
                        waited > std::chrono::milliseconds(300)) {
                        dup.NudgePresent();
                        firstFrameNudged = true;
                        Logf(ip, "no desktop present yet - nudged MTT/output for first frame\n");
                    } else if (!firstFrameWaitLogged &&
                               waited > std::chrono::milliseconds(2000)) {
                        dup.NudgePresent();
                        firstFrameWaitLogged = true;
                        Logf(ip, "still waiting for first GPU desktop present after nudge\n");
                    }
                }

                pendingCursorImg.clear();
                pendingCursor.clear();
                if (cap.pointerShapeChanged) {
                    PointerShapeBgra shape;
                    if (dup.GetPointerShapeBgra(shape)) {
                        pendingCursorImg = MakeCursorImgMessage(shape, dup.Width(), dup.Height());
                    }
                }
                if (cap.cursorChanged || !pendingCursorImg.empty()) {
                    const double invW = dup.Width() ? 1.0 / dup.Width() : 0.0;
                    const double invH = dup.Height() ? 1.0 / dup.Height() : 0.0;
                    const double cx = (dup.PointerX() + dup.PointerHotX()) * invW;
                    const double cy = (dup.PointerY() + dup.PointerHotY()) * invH;
                    ++cursorSeq;
                    pendingCursor = MakeCursorMessage(dup.PointerVisible(), cx, cy, cursorSeq);
                }

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
                const uint32_t wantW = Align16Ceil(dup.Width());
                const uint32_t wantH = Align16Ceil(dup.Height());
                // Capture/encoder WxH mismatch (incl. portrait swap). Require a
                // real desktop frame so Open() ModeDesc alone cannot fight the
                // encode size we just forced (that was the quick-flash loop).
                // Idle presents keep last frame size on dup, so pad-fail
                // mismatch still rebuilds without needing a fresh present.
                if (wantW && wantH && dup.HaveDesktopFrame() &&
                    (encoder.Width() != wantW || encoder.Height() != wantH)) {
                    const auto nowSz = std::chrono::steady_clock::now();
                    if (lastEncSizeRebuild != std::chrono::steady_clock::time_point{} &&
                        nowSz - lastEncSizeRebuild < std::chrono::milliseconds(250)) {
                        continue; // avoid rebuild thrash mid-flip
                    }
                    lastEncSizeRebuild = nowSz;
                    // Full rebuild (not Configure-in-place). If capture is the
                    // hello size swapped (portrait DXGI vs landscape hello),
                    // force MTT back to hello landscape and encode hello WxH -
                    // encoding portrait "fixes" flash but the iPad stays sideways.
                    Logf(ip, "capture is now %ux%u (encoder had %ux%u), rebuilding pipeline\n", dup.Width(),
                         dup.Height(), encoder.Width(), encoder.Height());
                    if (usingMttVdd)
                        (void)SaveMttDisplayTopologyIfUserChange(ip);
                    haveGpuDesktopFrame = false;
                    gpuNv12Path = false;
                    waitFirstFrameSince = {};
                    firstFrameNudged = false;
                    firstFrameWaitLogged = false;
                    const bool swappedHello = (wantW == height && wantH == width);
                    uint32_t rebuildW = wantW;
                    uint32_t rebuildH = wantH;
                    if (swappedHello) {
                        rebuildW = width;
                        rebuildH = height;
                        if (usingMttVdd) {
                            if (EnsureMonitorMode(mttMon.deviceName, rebuildW, rebuildH, kFps))
                                Logf(ip, "capture was portrait swap of hello - forced MTT %ux%u landscape\n",
                                     rebuildW, rebuildH);
                            else
                                Logf(ip, "capture was portrait swap of hello - MTT mode force failed\n");
                        }
                    } else {
                        width = wantW;
                        height = wantH;
                    }
                    if (!buildPipeline(rebuildW, rebuildH)) {
                        Logf(ip, "pipeline rebuild after resize failed, dropping the connection\n");
                        running = false;
                    } else {
                        width_ = dup.Width();
                        height_ = dup.Height();
                        if (GetMonitorRectByDeviceName(mttMon.deviceName, mttMon.rect))
                            input.SetMonitorRect(mttMon.rect);
                        continue; // capture a fresh frame on the new pipeline
                    }
                }

                auto now = std::chrono::steady_clock::now();

                // Persist Mike's MTT arrangement while streaming (DISPLAY index may change later).
                if (usingMttVdd &&
                    (lastMttPosPoll == std::chrono::steady_clock::time_point{} ||
                     now - lastMttPosPoll > std::chrono::seconds(2))) {
                    lastMttPosPoll = now;
                    PollMttDisplayTopology(ip, mttLastPos);
                }

                // Only adopt rotation into session size from a real frame - not
                // Open() ModeDesc, which can disagree with DXGI buffer orientation.
                // Foreign size after churn: log once (MTT does not ModeDesc-rebuild).
                if (changed && dup.HaveDesktopFrame() &&
                    dup.Width() == height && dup.Height() == width) {
                    std::swap(width, height); // rotated in Windows: that is the panel size now
                }
                // Exactly one log attempt after churn settles. MTT does not
                // ModeDesc-rebuild (fights capture-driven encode WxH).
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
                    // MTT: do not rebuild to ModeDesc - fights capture-driven encode WxH.
                    Logf(ip, "monitor sits at %ux%u (session %ux%u) - MTT: not forcing ModeDesc rebuild\n",
                         dup.Width(), dup.Height(), width, height);
                } else if (sizeRebuildDone && !sizeGiveUpLogged) {
                    // MTT modes are independent; mismatch usually means Display Settings
                    // is on a non-native size. Said once per connection.
                    sizeGiveUpLogged = true;
                    Logf(ip, "monitor stays at %ux%u (this iPad is %ux%u): pick %ux%u on the MTT monitor in "
                             "Display Settings - letterboxing means the desktop aspect doesn't match the panel\n",
                         dup.Width(), dup.Height(), width, height, width, height);
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

                // Keepalive re-encodes the last image (a tiny P-frame) so the
                // iPad's ~5s liveness watchdog (spec §5) never trips on an
                // otherwise idle desktop. GPU path only encodes once we have a
                // real present (CaptureFrameNv12Gpu returns null until then);
                // do not fall through to an empty CPU buffer.
                bool keepaliveDue = now - lastSend >= std::chrono::milliseconds(kKeepaliveMs);

                if (running && (active || keepaliveDue)) {
                    if (gpuNv12Path) {
                        // last-good texture from CaptureFrameNv12Gpu (or null
                        // before the first present / after Open reset).
                        if (gpuNv12) {
                            encoded = encoder.EncodeDxgiNv12(gpuNv12.Get());
                            ++cadenceEncCalls;
                            cadenceAus += static_cast<uint32_t>(encoded.size());
                        }
                    } else {
                        std::vector<uint8_t> encNv12;
                        if (!PadNv12TopLeft(nv12, dup.Width(), dup.Height(),
                                             encoder.Width(), encoder.Height(), encNv12)) {
                            const auto nowPad = std::chrono::steady_clock::now();
                            if (lastPadFailLog == std::chrono::steady_clock::time_point{} ||
                                nowPad - lastPadFailLog > std::chrono::milliseconds(2000)) {
                                lastPadFailLog = nowPad;
                                Logf(ip, "nv12 pad failed (cap %ux%u -> enc %ux%u)\n",
                                     dup.Width(), dup.Height(), encoder.Width(), encoder.Height());
                            }
                            continue;
                        }
                        encoded = encoder.EncodeNv12(encNv12.data(), encNv12.size());
                        ++cadenceEncCalls;
                        cadenceAus += static_cast<uint32_t>(encoded.size());
                    }
                    if (encoder.ConsecutiveAsyncTimeouts() >= 3) {
                        Logf(ip, "async encoder stalled (%d timeouts) - falling back to software\n",
                             encoder.ConsecutiveAsyncTimeouts());
                        gpuNv12Path = false;
                        encoder.SetAllowHardware(false);
                        if (!encoder.Configure(encoder.Width(), encoder.Height(), kFps,
                                               sessionEncode.peakBitrateBps, sessionEncode.qualityVsSpeed,
                                               sessionEncode.gopSeconds, sessionEncode.useQualityRc,
                                               sessionEncode.rcQuality)) {
                            Logf(ip, "software encoder fallback failed\n");
                        } else {
                            const std::wstring mft = encoder.MftName();
                            std::string mftA;
                            for (wchar_t c : mft)
                                mftA.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '?');
                            Logf(ip, "fallback MFT: %s\n", mftA.empty() ? "(unknown)" : mftA.c_str());
                        }
                    }
                }
            }

            // cursorImg always TCP. Positions: UDP when open; mirror TCP until
            // cursorAck (or forever if UDP never opens / times out).
            if (!pendingCursorImg.empty()) {
                if (!conn->SendFrame(reinterpret_cast<const uint8_t*>(pendingCursorImg.data()),
                                     static_cast<uint32_t>(pendingCursorImg.size()))) {
                    running = false;
                }
                pendingCursorImg.clear();
            }
            if (running && !pendingCursor.empty()) {
                bool udpOpen = false;
                bool confirmed = cursorUdpConfirmed.load(std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(cursorNetMutex);
                    udpOpen = cursorUdp.Ready();
                    if (udpOpen) {
                        cursorUdp.Send(reinterpret_cast<const uint8_t*>(pendingCursor.data()),
                                       pendingCursor.size());
                        if (!confirmed && !cursorUdpGaveUp &&
                            cursorUdpOpened != std::chrono::steady_clock::time_point{} &&
                            std::chrono::steady_clock::now() - cursorUdpOpened >
                                std::chrono::milliseconds(kCursorUdpAckMs)) {
                            cursorUdp.Close();
                            cursorUdpGaveUp = true;
                            udpOpen = false;
                            Logf(ip, "no cursorAck within %dms, falling back to TCP cursor\n",
                                 kCursorUdpAckMs);
                        }
                    }
                }
                const bool sendTcp = !udpOpen || !confirmed;
                if (sendTcp) {
                    if (!conn->SendFrame(reinterpret_cast<const uint8_t*>(pendingCursor.data()),
                                         static_cast<uint32_t>(pendingCursor.size()))) {
                        running = false;
                    }
                }
                pendingCursor.clear();
            }

            bool sentSomething = false;
            // Reply to receiver ping(s) before video so clock offset stays fresh.
            {
                std::vector<double> toPong;
                {
                    std::lock_guard<std::mutex> plock(pongMutex);
                    toPong.swap(pendingPongTs);
                }
                for (double tEcho : toPong) {
                    const std::string pong = MakePongMessage(tEcho, UnixEpochMs());
                    if (!conn->SendFrame(reinterpret_cast<const uint8_t*>(pong.data()),
                                         static_cast<uint32_t>(pong.size()))) {
                        running = false;
                        break;
                    }
                }
            }
            // Sender liveness ping every ~2s (PROTOCOL 8.2).
            if (running) {
                const auto nowPing = std::chrono::steady_clock::now();
                if (nowPing - lastSenderPing >= std::chrono::milliseconds(kSenderPingMs)) {
                    lastSenderPing = nowPing;
                    const std::string ping = MakeSenderPingMessage();
                    if (!conn->SendFrame(reinterpret_cast<const uint8_t*>(ping.data()),
                                         static_cast<uint32_t>(ping.size()))) {
                        running = false;
                    }
                }
            }

            // Dynamic: after a healthy stretch, step peak back toward the ceiling.
            // Never touches QualityVsSpeed (stays at the session preset value).
            if (sessionEncodeMode == EncodeMode::Dynamic &&
                sessionEncode.peakBitrateBps < sessionEncode.ceilingPeakBps) {
                const auto nowHealthy = std::chrono::steady_clock::now();
                if (nowHealthy - lastBackpressure >= std::chrono::milliseconds(kDynamicHealthyMs) &&
                    nowHealthy - lastDynamicAdjust >= std::chrono::milliseconds(kDynamicHealthyMs)) {
                    if (StepDynamicPeak(sessionEncode, /*congested=*/false)) {
                        if (encoder.UpdateBitrate(sessionEncode.peakBitrateBps,
                                                  sessionEncode.meanBitrateBps)) {
                            encoder.RequestKeyFrame();
                            if (nowHealthy - lastDynamicLog >= std::chrono::milliseconds(kDynamicLogMinMs)) {
                                lastDynamicLog = nowHealthy;
                                Logf(ip, "Dynamic encode: peak -> %u kbps (healthy)\n",
                                     sessionEncode.peakBitrateBps / 1000);
                            }
                        }
                        lastDynamicAdjust = nowHealthy;
                    }
                }
            }
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
                    lastBackpressure = std::chrono::steady_clock::now();
                    if (sessionEncodeMode == EncodeMode::Dynamic) {
                        if (StepDynamicPeak(sessionEncode, /*congested=*/true)) {
                            if (encoder.UpdateBitrate(sessionEncode.peakBitrateBps,
                                                      sessionEncode.meanBitrateBps)) {
                                encoder.RequestKeyFrame();
                                const auto nowAdj = std::chrono::steady_clock::now();
                                if (nowAdj - lastDynamicLog >= std::chrono::milliseconds(kDynamicLogMinMs)) {
                                    lastDynamicLog = nowAdj;
                                    Logf(ip, "Dynamic encode: peak -> %u kbps (backpressure)\n",
                                         sessionEncode.peakBitrateBps / 1000);
                                }
                            }
                            lastDynamicAdjust = std::chrono::steady_clock::now();
                        }
                    }
                    break;
                }
                const int64_t sndMs = UnixEpochMs();
                std::vector<uint8_t> wire = PrefixAnnexBWithTiming(f.annexB, capMs, sndMs);
                if (!conn->SendFrame(wire.data(), static_cast<uint32_t>(wire.size()))) {
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
            if (sentSomething) {
                ++cadenceSends;
                lastSend = std::chrono::steady_clock::now();
            }
        }

        conn->Close();
        reader.join();
        // After the reader is gone, so nothing else touches the injector.
        input.EndSession();
        // Drop DXGI hold on the virtual head before MTT detach (reload).
        dup.Close();
        bool lastClient = false;
        if (panel.held) {
            panel.held = false;
            lastClient = ReleasePanel();
        }
        if (lastClient && usingMttVdd) {
            // stopRequested_ => tray Disconnect / Exit / Stop (user-initiated).
            // Otherwise socket drop / transport flap: grace window, sticky VDD.
            const MttTeardownReason reason =
                stopRequested_ ? MttTeardownReason::UserInitiated : MttTeardownReason::LinkLoss;
            RequestMttVddTeardown(ip, reason);
            if (reason == MttTeardownReason::UserInitiated)
                usingMttVdd = false;
            mttModesEnsured = false;
            mttHelloModeForced = false;
        }
        width_ = 0;
        height_ = 0;
        transport_ = Transport::None;
        Logf(ip, "disconnected%s\n", stopRequested_ ? "" : ", reconnecting");
    }

    if (ipLock != nullptr)
        CloseHandle(ipLock);
    state_ = State::Idle;
}

} // namespace od

