#include "app/SenderApp.h"

#include "app/Log.h"
#include "app/Config.h"
#include "app/EncodeProfile.h"
#include "app/PauseSignals.h"
#include "display/PauseOverlay.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <climits>
#include <algorithm>
#include <set>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <winsock2.h>
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include "display/DesktopDuplication.h"
#include "display/DisplayBackend.h"

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
constexpr int kNoHelloBackoffMinMs = 500;    // first retry after a link that closed before hello
constexpr int kNoHelloBackoffMaxMs = 30000;  // cap: an iPad with the app closed is checked twice a minute

// Exponential backoff for links that accept and close without a hello (iPad
// app backgrounded, usbmux forward to a closed port): 0.5s, 1s, 2s ... 30s.
int NoHelloBackoffMs(int failures)
{
    int ms = kNoHelloBackoffMinMs;
    for (int i = 1; i < failures && ms < kNoHelloBackoffMaxMs; ++i)
        ms *= 2;
    return (std::min)(ms, kNoHelloBackoffMaxMs);
}
constexpr int kKeepaliveMs = 1000;       // max silence on a static screen; well under the iPad's ~5s watchdog
constexpr int kWrongSizeGraceMs = 3000;  // how long the monitor may sit on a foreign size before we rebuild it
constexpr int kBlockedRetryMs = 5000;    // how often a waiting iPad checks whether the display is free again
constexpr int kCursorUdpAckMs = 5000;    // drop UDP and stay on TCP if no cursorAck (PROTOCOL 6.3)

// Only one panel size may be on the air at a time.
//
// parsec-vdd puts a single custom resolution on all of its virtual monitors:
// whichever sender sets its size last drags every other monitor along, and
// those iPads then show a letterboxed desktop at the wrong aspect. MTT has one
// shared head whose modes come from one iPad's EDID. Either way, iPads of the
// same panel size run together and a different one waits until the display
// is free again.
//
// Process-wide, because the tray drives every sender. A headless CLI sender
// started next to the tray is outside this and can still take the mode with
// it — that path is for testing.
struct PanelState {
    std::mutex mutex;
    uint32_t width = 0;
    uint32_t height = 0;
    int holders = 0;
    bool exclusive = false;     // MTT: one iPad on the head at a time
    const void* first = nullptr; // sender that took the panel first (for "in use by")
};

PanelState& Panel()
{
    static PanelState state;
    return state;
}

// Takes a share of the display for w x h. Returns false when a different size
// holds it - or, exclusive (MTT: one head), when anyone holds it - and then
// reports that size in activeW/activeH and the holding sender in holder.
bool AcquirePanel(uint32_t w, uint32_t h, bool exclusive, const void* self, uint32_t& activeW, uint32_t& activeH,
                  const void*& holder)
{
    PanelState& panel = Panel();
    std::lock_guard<std::mutex> lock(panel.mutex);
    if (panel.holders > 0 && (exclusive || panel.exclusive || panel.width != w || panel.height != h)) {
        activeW = panel.width;
        activeH = panel.height;
        holder = panel.first;
        return false;
    }
    if (panel.holders == 0) {
        panel.exclusive = exclusive;
        panel.first = self;
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
        panel.exclusive = false;
        panel.first = nullptr;
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

// 1 ms system timer resolution for the length of a streaming session: a
// backstop so any remaining Sleep()/timed wait on the capture path is not
// rounded up to 15.6 ms. Released when the session ends.
struct TimerResolution1ms {
    bool on = timeBeginPeriod(1) == TIMERR_NOERROR;
    ~TimerResolution1ms()
    {
        if (on)
            timeEndPeriod(1);
    }
    TimerResolution1ms() = default;
    TimerResolution1ms(const TimerResolution1ms&) = delete;
    TimerResolution1ms& operator=(const TimerResolution1ms&) = delete;
};

// One USB cable feeds one sender: serials (or "#deviceId") currently dialed.
std::mutex g_usbClaimMutex;
std::set<std::string> g_usbClaims;

std::string UsbClaimKey(const UsbMuxDevice& d)
{
    return d.serial.empty() ? "#" + std::to_string(d.deviceId) : d.serial;
}

struct UsbClaim {
    std::string key;
    bool Take(const std::string& k)
    {
        std::lock_guard<std::mutex> lock(g_usbClaimMutex);
        if (!g_usbClaims.insert(k).second)
            return false;
        key = k;
        return true;
    }
    ~UsbClaim()
    {
        if (key.empty())
            return;
        std::lock_guard<std::mutex> lock(g_usbClaimMutex);
        g_usbClaims.erase(key);
    }
};

// Which USB iPad belongs to this sender. expectedId is the entry's Bonjour
// TXT id (or the id its last hello gave). A device is ours when the
// serial->id we learned from an earlier USB hello equals it. Without such a
// match (first USB session for this iPad) a single never-seen device is tried
// unless strict; the hello then confirms it. No id at all: only when exactly
// Entries without an id (typed-in IP) never use USB: the cable could carry any
// iPad. Devices other senders hold are never picked.
// needsVerify: the pick was a guess and hello.id must confirm it.
bool PickUsbForEntry(const std::string& expectedId, const std::string& skipSerial, bool strict, UsbMuxDevice& out,
                     bool& needsVerify)
{
    std::vector<UsbMuxDevice> devices;
    if (!ListUsbMuxDevices(devices) || devices.empty())
        return false;
    const Config cfg = Config::Load();
    std::vector<UsbMuxDevice> free;
    {
        std::lock_guard<std::mutex> lock(g_usbClaimMutex);
        for (const UsbMuxDevice& d : devices)
            if (!g_usbClaims.count(UsbClaimKey(d)))
                free.push_back(d);
    }
    needsVerify = false;
    if (!expectedId.empty()) {
        for (const UsbMuxDevice& d : free) {
            if (!d.serial.empty() && cfg.IdForUsbSerial(d.serial) == expectedId) {
                out = d;
                return true;
            }
        }
        if (strict)
            return false;
        const UsbMuxDevice* unknown = nullptr;
        int unknownCount = 0;
        for (const UsbMuxDevice& d : free) {
            if (d.serial == skipSerial && !skipSerial.empty())
                continue;
            if (d.serial.empty() || cfg.IdForUsbSerial(d.serial).empty()) {
                unknown = &d;
                ++unknownCount;
            }
        }
        if (unknownCount != 1)
            return false;
        out = *unknown;
        needsVerify = true;
        return true;
    }
    return false;
}

} // namespace

namespace {
std::atomic<HWND> g_statusHwnd{nullptr};
std::atomic<UINT> g_statusMsg{0};
std::atomic<bool> g_statusPosted{false};
} // namespace

void SenderApp::SetStatusWindow(void* hwnd, unsigned msg)
{
    g_statusMsg = msg;
    g_statusHwnd = static_cast<HWND>(hwnd);
}

void SenderApp::StatusMessageHandled()
{
    g_statusPosted = false;
}

void SenderApp::NotifyStatus()
{
    const HWND hwnd = g_statusHwnd.load();
    if (!hwnd || g_statusPosted.exchange(true))
        return;
    if (!PostMessageW(hwnd, g_statusMsg.load(), 0, 0))
        g_statusPosted = false;
}

SenderApp::~SenderApp()
{
    Stop();
}

void SenderApp::Start(std::string ip, uint16_t port, std::string entryId)
{
    if (running_.exchange(true))
        return; // already running
    entryId_ = std::move(entryId);

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
    SetState(State::Idle);
    SetTransport(Transport::None);
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
        SetState(State::Idle);
        return;
    }

    // Declaration order matters for teardown: H264Encoder's ctor initializes
    // COM (MTA) + Media Foundation for this thread and its dtor uninitializes
    // them. Locals are destroyed in reverse, so the encoder is declared FIRST
    // (destroyed LAST) — otherwise DesktopDuplication's D3D11/DXGI COM objects
    // would be released after CoUninitialize(), an access violation on Stop().
    H264Encoder encoder;
    // Virtual display driver behind the head (MTT or Parsec, see
    // DisplayBackend.h). Between encoder and dup on purpose: the duplication
    // must be released before the head goes away. Picked per connection
    // attempt from config (displayBackend), so a Settings change applies on
    // the next connect.
    std::unique_ptr<DisplayBackend> backend;
    DesktopDuplication dup;
    InputInjector input;
    std::mutex pipelineMutex;
    bool gpuNv12Path = false; // VideoProcessor BGRA->NV12 + EncodeDxgiNv12
    bool haveGpuDesktopFrame = false; // true after first successful GPU present

    // Updated once the dial settles (USB vs Wi-Fi); buildPipeline / reconfigure read these.
    EncodeKnobs sessionEncode;
    EncodeMode sessionEncodeMode = EncodeMode::Dynamic;
    EncodePreset sessionEncodePreset = EncodePreset::Speed;

    // Bumped on every (re)build - duplication rebuilt, mode may have changed.
    std::atomic<uint32_t> pipelineGen{0};
    auto buildPipeline = [&](uint32_t width, uint32_t height, BuildReason reason = BuildReason::Hello) {
        // Caller holds pipelineMutex.
        ++pipelineGen;
        dup.Close();
        if (!backend || !backend->Prepare(width, height, reason))
            return false;
        const std::wstring headName = backend->DeviceName();
        if (!dup.Open(headName)) {
            Logf(ip, "DesktopDuplication::Open failed on %ls\n", headName.c_str());
            return false;
        }
        {
            const RECT r = backend->CurrentRect();
            input.SetMonitorRect(r);
            Logf(ip, "input rect %ls L=%ld T=%ld R=%ld B=%ld\n", headName.c_str(), r.left, r.top, r.right,
                 r.bottom);
        }
        // Bind encoder here (not later on size-flip with HW disabled): QSV needs
        // an Intel D3D device + SET_D3D_MANAGER before the first Configure.
        // Prefer the capture device when it is Intel so VideoProcessor
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
            // Share the capture device with the encoder whatever its vendor;
            // without one, a separate Intel device still serves QSV.
            bool haveEncoderD3d = false;
            if (dup.Device() && dup.Context() && encoder.AdoptD3DDevice(dup.Device(), dup.Context()))
                haveEncoderD3d = true;
            else if (encoder.EnsureIntelEncoderDevice())
                haveEncoderD3d = true;
            if (!haveEncoderD3d)
                Logf(ip, "no D3D device for the encoder - HW encode may use system memory or fall back\n");
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
        // The probe in Configure used up its forced IDR. The GPU path asks for
        // one on its first present; the CPU path must ask here, or the iPad
        // gets P-frames until the next GOP after a reconnect or rotation.
        if (!gpuNv12Path)
            encoder.RequestKeyFrame();
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
    int noHelloFailures = 0;    // consecutive links that closed before a hello (backoff)
    // USB matching (see PickUsbForEntry): the id this entry's iPad presents.
    std::string knownId = entryId_;
    bool forceWifiOnce = false;           // last USB pick was the wrong iPad
    std::string rejectedSerial;           // ... and is skipped for a minute
    std::chrono::steady_clock::time_point rejectedSerialUntil{};

    bool stickToUsbAfterSession = false;
    // An entry without an id (typed-in IP) is Wi-Fi only: whatever iPad is on
    // the cable is not necessarily the one at that address.
    const bool usbAllowed = !entryId_.empty();
    // USB dial/hello failures on this iPad's cable: the connect path and
    // Back-to-USB skip USB until the backoff runs out, so a cable whose app is
    // not listening does not bounce the Wi-Fi session every few seconds.
    int usbFailCount = 0;
    std::chrono::steady_clock::time_point usbFailUntil{};
    auto noteUsbFailure = [&](const std::string& serial) {
        usbFailCount = (std::min)(usbFailCount + 1, 6);
        const int sec = 10 << (usbFailCount - 1); // 10 s .. 320 s
        usbFailUntil = std::chrono::steady_clock::now() + std::chrono::seconds(sec);
        Logf(ip, "USB %s failed (%d in a row) - Wi-Fi only for %d s\n", serial.empty() ? "?" : serial.c_str(),
             usbFailCount, sec);
    };

while (!stopRequested_) {
        // A blocked sender keeps checking back every few seconds; flipping it to
        // Connecting for each of those attempts would make the tray entry
        // alternate between "waiting for 2732x2048" and "connecting..." while
        // nothing about its situation changed.
        if (state_ != State::Blocked)
            SetState(State::Connecting);

        // USB preferred while usbmux + a USB device are available.
        // usbWifiFailover (default ON) keeps today's behavior: if USB dial
        // fails or the cable drops after a USB session, fall back to Wi-Fi.
        // When OFF, after a USB session we wait for USB again instead of
        // staying on Wi-Fi; Wi-Fi-only iPads (never USB this run) still work.
        const Config connectCfg = Config::Load();
        const bool allowWifiFailover = connectCfg.usbWifiFailover;

        // Display driver for this attempt. An explicit choice whose driver is
        // missing is reported (tray + log) and waited on, never swapped.
        {
            const DisplayBackendSelection sel = SelectDisplayBackend(connectCfg.displayBackend);
            if (!sel.driverInstalled) {
                SetStatusDetail(sel.problem);
                if (state_ != State::DriverMissing)
                    Logf(ip, "display: %s - not connecting until it is installed or Settings change\n",
                         sel.problem.c_str());
                SetState(State::DriverMissing);
                InterruptibleSleep(kBlockedRetryMs);
                continue;
            }
            if (state_ == State::DriverMissing)
                SetState(State::Connecting);
            SetStatusDetail({});
            if (!backend || backend->Kind() != sel.kind) {
                std::lock_guard<std::mutex> lock(pipelineMutex);
                dup.Close();
                backend.reset();
                backend = MakeDisplayBackend(sel.kind, ip, kFps, &stopRequested_);
                Logf(ip, "display backend: %s (%s)\n", DisplayBackendName(sel.kind),
                     sel.automatic ? "auto" : "selected in Settings");
            }
        }

        bool usbSession = false;
        std::optional<Connection> conn;
        UsbMuxDevice usbDev{};
        UsbClaim usbClaim;          // released when this attempt ends
        bool usbNeedsVerify = false;
        const bool configuredLoopback = IsUsbLikeHost(ip);
        const bool skipUsb = forceWifiOnce ||
                             (allowWifiFailover && usbFailCount > 0 && std::chrono::steady_clock::now() < usbFailUntil);
        forceWifiOnce = false;
        const std::string skipSerial =
            std::chrono::steady_clock::now() < rejectedSerialUntil ? rejectedSerial : std::string{};
        if (configuredLoopback) {
            // Hand-entered 127.0.0.1: the localhost forward (first USB device).
            (void)EnsureUsbMuxForward(port, kUsbMuxLocalPort);
        } else if (usbAllowed && !skipUsb && PickUsbForEntry(knownId, skipSerial, false, usbDev, usbNeedsVerify)) {
            if (!usbClaim.Take(UsbClaimKey(usbDev))) {
                Logf(ip, "USB device %s was just taken by another iPad entry, using Wi-Fi\n", usbDev.serial.c_str());
            } else {
                Logf(ip, "USB usbmux device %s (id %u%s), dialing it directly\n",
                     usbDev.serial.empty() ? "?" : usbDev.serial.c_str(), usbDev.deviceId,
                     usbNeedsVerify ? ", unconfirmed - checking hello.id" : ", matches learned id");
                if (auto sock = ConnectUsbMux(usbDev.deviceId, port))
                    conn = Connection::FromSocket(*sock);
                if (conn) {
                    usbSession = true;
                    Logf(ip, "connected via USB (usbmux device %u -> iPad:%u)\n", usbDev.deviceId, port);
                } else if (!allowWifiFailover) {
                    Logf(ip, "USB usbmux dial failed, Wi-Fi failover off, retrying USB\n");
                    InterruptibleSleep(kReconnectDelayMs);
                    continue;
                } else {
                    Logf(ip, "USB usbmux dial failed, falling back to Wi-Fi %s:%u\n", ip.c_str(), port);
                    noteUsbFailure(usbDev.serial);
                }
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
        SetTransport(usbSession ? Transport::Usb : Transport::Wifi);
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
        if (!gotHello) {
            conn->Close();
            if (stopRequested_)
                continue;
            ++noHelloFailures;
            if (usbSession && allowWifiFailover)
                noteUsbFailure(usbDev.serial);
            const int waitMs = NoHelloBackoffMs(noHelloFailures);
            // Log the first few and then only when the delay changes (the cap).
            if (noHelloFailures <= 3 || waitMs != NoHelloBackoffMs(noHelloFailures - 1))
                Logf(ip, "link closed before hello (%d in a row), retrying in %dms\n", noHelloFailures, waitMs);
            InterruptibleSleep(waitMs);
            continue;
        }
        noHelloFailures = 0;
        if (usbSession)
            usbFailCount = 0;

        // Re-learn USB serial <-> hello.id on every USB hello (the id changes
        // when the iPad app is reinstalled), so matching and Settings stay right.
        if (usbSession && !usbDev.serial.empty() && !hello.id.empty()) {
            Config learned = Config::Load();
            if (learned.IdForUsbSerial(usbDev.serial) != hello.id) {
                learned.RememberUsbSerialId(usbDev.serial, hello.id);
                learned.Save();
                Logf(ip, "USB serial %s bound to id %s\n", usbDev.serial.c_str(), hello.id.c_str());
            }
        }
        // The cable we dialed must be this entry's iPad.
        if (usbSession && !knownId.empty() && !hello.id.empty() && hello.id != knownId) {
            Logf(ip, "USB device %s is iPad id %s, not this entry's %s - closing it and using Wi-Fi\n",
                 usbDev.serial.empty() ? "?" : usbDev.serial.c_str(), hello.id.c_str(), knownId.c_str());
            conn->Close();
            rejectedSerial = usbDev.serial;
            rejectedSerialUntil = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            forceWifiOnce = true;
            continue;
        }
        if (entryId_.empty() && !hello.id.empty())
            knownId = hello.id; // no TXT id configured: follow the iPad we actually reached

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
        // Raw hello size (unaligned): MTT generates the per-iPad EDID from it.
        backend->BeginConnection(hello.pixelsWide > 0 ? static_cast<uint32_t>(hello.pixelsWide) : 0,
                                 hello.pixelsHigh > 0 ? static_cast<uint32_t>(hello.pixelsHigh) : 0);

        // Only iPads of the panel size that is already on the air may join (see
        // AcquirePanel). A different one hangs up and keeps checking back, so
        // it starts by itself once the other iPad disconnects.
        PanelHolder panel;
        TimerResolution1ms timerResolution;
        uint32_t activeWidth = 0, activeHeight = 0;
        const bool exclusiveHead = backend->Kind() == DisplayBackendKind::Mtt;
        const void* holder = nullptr;
        panel.held = AcquirePanel(width, height, exclusiveHead, this, activeWidth, activeHeight, holder);
        if (!panel.held) {
            blockedByWidth_ = activeWidth;
            blockedByHeight_ = activeHeight;
            blockedByApp_ = holder;
            blockedExclusive_ = exclusiveHead;
            NotifyStatus(); // the holder may change while we stay Blocked
            SetState(State::Blocked);
            // Once per episode, not on every check: this comes back every few
            // seconds for as long as the other iPad streams.
            if (!blockedLogged) {
                blockedLogged = true;
                if (exclusiveHead)
                    Logf(ip, "waiting: another iPad (%ux%u) is on the MTT display - MTT drives one iPad at a "
                             "time (Parsec supports several)\n",
                         activeWidth, activeHeight);
                else
                    Logf(ip, "waiting: an iPad with a %ux%u panel is streaming and this one is %ux%u - %s only "
                             "holds one panel size at a time\n",
                         activeWidth, activeHeight, width, height, DisplayBackendName(backend->Kind()));
            }
            conn->Close();
            InterruptibleSleep(kBlockedRetryMs);
            continue;
        }
        blockedByWidth_ = 0;
        blockedByHeight_ = 0;
        blockedByApp_ = nullptr;
        blockedExclusive_ = false;
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
                backend->OnPipelineFailed(ReleasePanel());
            }
            InterruptibleSleep(kReconnectDelayMs);
            continue;
        }

        width_ = width;
        height_ = height;
        SetState(State::Streaming);
        backend->OnStreaming();

        std::atomic<bool> running{true};
        bool loggedPencil = false; // reader-thread only; one line per connection

        // UDP cursor side channel (PROTOCOL 6.3). Declared before the reader so
        // cursorAck can mark the channel confirmed. Positions get a session seq;
        // mirror on TCP until ack, then UDP-only. cursorImg stays TCP.
        CursorUdp cursorUdp;
        std::mutex cursorNetMutex;
        // The receiver's rate envelope (displayMaxFrameRate + videoCaps) from
        // the latest hello; the reader updates it on every re-hello and bumps
        // rateHelloGen so the capture loop recomputes its frame-rate limit.
        std::mutex rateHelloMutex;
        HelloMsg rateHello = hello;
        std::atomic<uint32_t> rateHelloGen{1};
        std::atomic<bool> cursorUdpConfirmed{false};
        std::atomic<bool> cursorForce{false}; // shape changed: resend position now
        uint64_t cursorSeq = 0;                // under cursorNetMutex
        std::chrono::steady_clock::time_point cursorUdpOpened{};
        bool cursorUdpGaveUp = false;

                // Receiver ping → pong (PROTOCOL 8.1). Reader must not write the
        // socket (interleaves with video); queue t values for the capture loop.
        std::mutex pongMutex;
        std::vector<double> pendingPongTs;
        auto lastSenderPing = std::chrono::steady_clock::now();
        constexpr int kSenderPingMs = 2000;

        // Touch/pen/scroll target the capture head; follow live moves.
        RECT lastLoggedInputRect{};
        bool loggedInputRect = false;
        auto refreshInputRect = [&]() {
            const RECT r = backend->CurrentRect();
            input.SetMonitorRect(r);
            if (!loggedInputRect || r.left != lastLoggedInputRect.left || r.top != lastLoggedInputRect.top ||
                r.right != lastLoggedInputRect.right || r.bottom != lastLoggedInputRect.bottom) {
                Logf(ip, "input rect L=%ld T=%ld R=%ld B=%ld\n", r.left, r.top, r.right, r.bottom);
                lastLoggedInputRect = r;
                loggedInputRect = true;
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
                        {
                            std::lock_guard<std::mutex> rlock(rateHelloMutex);
                            rateHello = msg->hello;
                        }
                        ++rateHelloGen;
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
        auto lastSend = std::chrono::steady_clock::now();
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
                ++cursorSeq; // cursorNetMutex held
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

        // Cursor thread: polls GetCursorInfo at the head's refresh rate and
        // sends positions on its own, so the pointer is not tied to the
        // encode loop (which runs at the capture/encode cadence, ~30 fps on a
        // busy link). UDP when open; mirrored on TCP until cursorAck, or for
        // good on USB / when UDP never confirms. SendFrame is thread-safe.
        std::atomic<bool> cursorStop{false};
        std::atomic<bool> cursorPaused{false}; // pause mode: no cursor updates
        std::thread cursorThread([&] {
            HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                  TIMER_ALL_ACCESS);
            // Refresh re-read only when the pipeline is rebuilt or the tray saw
            // WM_DISPLAYCHANGE (same counters as the frame limiter).
            uint32_t hz = backend->RefreshHz();
            uint32_t seenPipelineGen = pipelineGen.load();
            uint32_t seenDisplayGen = PauseSignals::displayChangeGen.load();
            bool haveLast = false;
            bool lastVisible = false;
            LONG lastX = 0, lastY = 0;
            while (!cursorStop && running && !stopRequested_) {
                if (pipelineGen.load() != seenPipelineGen ||
                    PauseSignals::displayChangeGen.load() != seenDisplayGen) {
                    seenPipelineGen = pipelineGen.load();
                    seenDisplayGen = PauseSignals::displayChangeGen.load();
                    hz = backend->RefreshHz();
                }
                const uint32_t tickHz = (std::max)(30u, (std::min)(hz, 240u));
                const RECT r = backend->CurrentRect();
                CURSORINFO ci{};
                ci.cbSize = sizeof(ci);
                const bool gotInfo = GetCursorInfo(&ci) != FALSE;
                const LONG rw = r.right - r.left, rh = r.bottom - r.top;
                // ptScreenPos is already the hotspot, in desktop coordinates.
                const bool inside = gotInfo && rw > 0 && rh > 0 && ci.ptScreenPos.x >= r.left &&
                                    ci.ptScreenPos.x < r.right && ci.ptScreenPos.y >= r.top &&
                                    ci.ptScreenPos.y < r.bottom;
                const bool visible = inside && (ci.flags & CURSOR_SHOWING) != 0;
                const LONG x = inside ? ci.ptScreenPos.x - r.left : lastX;
                const LONG y = inside ? ci.ptScreenPos.y - r.top : lastY;
                const bool force = cursorForce.exchange(false);
                if (!cursorPaused && gotInfo && (force || !haveLast || visible != lastVisible || (visible && (x != lastX || y != lastY)))) {
                    haveLast = true;
                    lastVisible = visible;
                    lastX = x;
                    lastY = y;
                    bool udpOpen = false;
                    bool sendTcp = true;
                    std::string msg;
                    {
                        std::lock_guard<std::mutex> lock(cursorNetMutex);
                        ++cursorSeq;
                        msg = MakeCursorMessage(visible, static_cast<double>(x) / rw, static_cast<double>(y) / rh,
                                                cursorSeq);
                        const bool confirmed = cursorUdpConfirmed.load(std::memory_order_relaxed);
                        udpOpen = cursorUdp.Ready();
                        if (udpOpen) {
                            cursorUdp.Send(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
                            if (!confirmed && !cursorUdpGaveUp &&
                                cursorUdpOpened != std::chrono::steady_clock::time_point{} &&
                                std::chrono::steady_clock::now() - cursorUdpOpened >
                                    std::chrono::milliseconds(kCursorUdpAckMs)) {
                                cursorUdp.Close();
                                cursorUdpGaveUp = true;
                                udpOpen = false;
                                Logf(ip, "no cursorAck within %dms, falling back to TCP cursor\n", kCursorUdpAckMs);
                            }
                        }
                        sendTcp = !udpOpen || !confirmed;
                    }
                    if (sendTcp &&
                        !conn->SendFrame(reinterpret_cast<const uint8_t*>(msg.data()), static_cast<uint32_t>(msg.size())))
                        running = false;
                }
                const LONGLONG periodNs100 = 10'000'000LL / tickHz;
                if (timer) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -periodNs100;
                    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                        WaitForSingleObject(timer, 100);
                    else
                        Sleep(1);
                } else {
                    Sleep(1);
                }
            }
            if (timer)
                CloseHandle(timer);
        });

        // Back to USB: on a Wi-Fi session, look for this iPad's cable every
        // few seconds (strict match on the learned serial->id only). When it
        // shows up, end this session; the reconnect prefers USB and MTT keeps
        // its head through the link-loss grace, so the picture comes back on
        // the same display.
        std::atomic<bool> movedToUsb{false};
        std::thread usbWatch;
        if (!usbSession && !configuredLoopback && usbAllowed && !knownId.empty()) {
            usbWatch = std::thread([&] {
                int ticks = 0;
                while (!cursorStop && running && !stopRequested_) {
                    Sleep(100);
                    if (++ticks % 30 != 0 || state_ != State::Streaming)
                        continue;
                    UsbMuxDevice d;
                    bool verify = false;
                    // Set before this session started; read-only while it runs.
                    if (usbFailCount > 0 && std::chrono::steady_clock::now() < usbFailUntil)
                        continue;
                    if (PickUsbForEntry(knownId, {}, true, d, verify)) {
                        Logf(ip, "USB cable for this iPad is back (%s) - moving the session from Wi-Fi to USB\n",
                             d.serial.c_str());
                        movedToUsb = true;
                        running = false;
                        conn->Close(); // unblock the reader
                        break;
                    }
                }
            });
        }

        std::chrono::steady_clock::time_point wrongSizeSince{};
        bool sizeRebuildDone = false;
        bool sizeGiveUpLogged = false;
        std::chrono::steady_clock::time_point waitFirstFrameSince{};
        bool firstFrameNudged = false;
        bool firstFrameWaitLogged = false;
        std::chrono::steady_clock::time_point lastEncSizeRebuild{};
        std::chrono::steady_clock::time_point lastPadFailLog{};
        // Local cadence: presents / encode-calls / AUs / sends per window.
        std::chrono::steady_clock::time_point cadenceSince = std::chrono::steady_clock::now();
        uint32_t cadencePresents = 0;
        uint32_t cadenceEncCalls = 0;
        // Frame-rate limiter (no sleeping): at most one encode per slot of
        // 1/fpsLimit, on an anchored schedule. A frame that arrives early is
        // held as "pending" (the capture's NV12 buffer/texture always holds
        // the newest one) and AcquireNextFrame's timeout is shortened to the
        // slot, so it goes out on time - the only wait is that existing call.
        using Clock = std::chrono::steady_clock; // QPC on Windows
        constexpr int kIdleCaptureTimeoutMs = 1000 / static_cast<int>(kFps);
        uint32_t fpsLimit = 60;
        uint32_t fpsLimitGen = 0;
        uint32_t fpsLimitW = 0, fpsLimitH = 0;
        // Head refresh, read at stream start and again only when something says
        // it may have changed: a pipeline rebuild (ACCESS_LOST, mode change,
        // pause resume, rotation) or a WM_DISPLAYCHANGE from the tray.
        uint32_t fpsLimitHz = backend->RefreshHz();
        uint32_t hzSeenPipelineGen = pipelineGen.load();
        uint32_t hzSeenDisplayGen = PauseSignals::displayChangeGen.load();
        Clock::duration slot = std::chrono::microseconds(1'000'000 / 60);
        Clock::duration earlyWindow = slot / 4;
        Clock::time_point nextSlot{};
        bool framePending = false;
        // Tail: the async MFT can hold the last image's AU until another input
        // arrives. One re-encode a slot after the last new image pushes it out
        // (otherwise it could sit until the 1 s keepalive). Scheduled, not slept.
        bool tailPending = false;
        Clock::time_point tailAt{};
        uint32_t cadenceAus = 0;
        uint32_t cadenceSends = 0;
        uint32_t cadenceTimeouts = 0;

        // Pause mode: a locked PC, a dark console display or the secure desktop
        // (UAC) stop capture, encode and cursor updates, but the connection,
        // the MTT head and the session stay. The iPad gets one overlay frame
        // (last picture dimmed + reason card), re-sent as the keepalive. Resume
        // rebuilds duplication on the same output and starts on an IDR.
        bool paused = false;
        bool pauseFromSecureDesktop = false; // entered on ACCESS_LOST with the secure desktop up
        PauseReason pauseReason = PauseReason::None;
        std::vector<uint8_t> pauseBase;      // BGRA under the overlay
        uint32_t pauseBaseW = 0, pauseBaseH = 0;
        std::vector<uint8_t> pauseNv12;      // overlay at encoder size
        int pauseEncodesDue = 0;             // overlay encodes owed right now
        Clock::time_point lastResumeTry{};
        bool resumeFailLogged = false;

        auto pauseReasonNow = [&](InputDesktop desk) {
            // The lock event starts it; WTS session state decides while it lasts
            // (also catches a lock the tray never heard about, or a missed unlock).
            const bool lockEvent = PauseSignals::locked.load();
            if ((lockEvent || desk != InputDesktop::Default) && IsSessionLocked())
                return PauseReason::Locked;
            if (PauseSignals::displayOff.load())
                return PauseReason::Asleep;
            if (desk == InputDesktop::Secure)
                return PauseReason::Admin;
            if (desk == InputDesktop::Other)
                return PauseReason::Paused;
            return PauseReason::None;
        };
        // Overlay frame for `reason` from pauseBase, padded to the encoder size.
        auto renderPauseFrame = [&](PauseReason reason) {
            std::vector<uint8_t> img = pauseBase;
            uint32_t w = pauseBaseW, h = pauseBaseH;
            if (img.empty() || w > encoder.Width() || h > encoder.Height()) {
                w = encoder.Width();
                h = encoder.Height();
                img.assign(static_cast<size_t>(w) * h * 4, 0x30); // neutral dark gray
            }
            if (!RenderPauseOverlay(img.data(), w, h, reason))
                Logf(ip, "pause overlay draw failed - sending the dimmed-less picture\n");
            std::vector<uint8_t> nv12Small;
            ConvertBgraToNv12Cpu(img.data(), w * 4, w, h, nv12Small);
            if (!PadNv12TopLeft(nv12Small, w, h, encoder.Width(), encoder.Height(), pauseNv12))
                pauseNv12.clear();
            pauseReason = reason;
            pauseEncodesDue = 2; // the async MFT hands out an AU on the next input
            encoder.RequestKeyFrame();
        };
        auto enterPause = [&](PauseReason reason, bool fromSecure) {
            paused = true;
            pauseFromSecureDesktop = fromSecure;
            cursorPaused = true;
            framePending = false;
            tailPending = false;
            if (!dup.LastFrameBgra(pauseBase, pauseBaseW, pauseBaseH))
                pauseBase.clear();
            renderPauseFrame(reason);
            resumeFailLogged = false;
            NotifyStatus();
            Logf(ip, "paused (%s): capture/encode stopped, connection kept, overlay %s\n", PauseReasonName(reason),
                 pauseNv12.empty() ? "unavailable" : "sent");
        };

        while (running && !stopRequested_) {
            std::vector<EncodedFrame> encoded;
            int64_t capMs = 0;
            // Recompute the limit on a new hello, a new encoded raster or a
            // new refresh rate on the head (re-read on rebuild / display change).
            bool hzChanged = false;
            if (pipelineGen.load() != hzSeenPipelineGen ||
                PauseSignals::displayChangeGen.load() != hzSeenDisplayGen) {
                hzSeenPipelineGen = pipelineGen.load();
                hzSeenDisplayGen = PauseSignals::displayChangeGen.load();
                const uint32_t hzNow = backend->RefreshHz();
                hzChanged = hzNow != fpsLimitHz;
                fpsLimitHz = hzNow;
            }
            if (hzChanged || rateHelloGen.load() != fpsLimitGen || encoder.Width() != fpsLimitW ||
                encoder.Height() != fpsLimitH) {
                fpsLimitGen = rateHelloGen.load();
                fpsLimitW = encoder.Width();
                fpsLimitH = encoder.Height();
                HelloMsg rh;
                {
                    std::lock_guard<std::mutex> rlock(rateHelloMutex);
                    rh = rateHello;
                }
                // Never above the rate the encoder is configured for (kFps:
                // timestamps, bitrate and GOP assume it) or the head's refresh.
                const uint32_t receiverLimit = StreamFpsLimit(rh, "h264", fpsLimitW, fpsLimitH);
                uint32_t limit = (std::min)(receiverLimit, kFps);
                if (fpsLimitHz > 0)
                    limit = (std::min)(limit, fpsLimitHz);
                limit = (std::max)(limit, 1u);
                if (limit != fpsLimit || fpsLimitGen == 1) {
                    uint32_t capsRate = 0;
                    for (const VideoCapEntry& e : rh.videoCaps)
                        if (e.codec == "h264")
                            capsRate = (std::max)(capsRate, e.maxFrameRate);
                    Logf(ip, "fps limit %u (receiver %u: displayMaxFrameRate=%u, h264 videoCaps maxFrameRate=%u, %zu caps; "
                             "encoder %u, head %u Hz, %ux%u)\n",
                         limit, receiverLimit, rh.displayMaxFrameRate, capsRate, rh.videoCaps.size(), kFps, fpsLimitHz,
                         fpsLimitW, fpsLimitH);
                }
                fpsLimit = limit;
                slot = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fpsLimit));
                earlyWindow = slot / 4;
            }
            // Capture timeout: up to the next slot while a frame waits,
            // otherwise the normal idle cadence.
            int captureTimeoutMs = kIdleCaptureTimeoutMs;
            if (framePending || tailPending) {
                const auto due = framePending ? nextSlot : tailAt;
                const auto left = due - Clock::now();
                const auto leftMs = std::chrono::ceil<std::chrono::milliseconds>(left).count();
                captureTimeoutMs = static_cast<int>((std::max)(0LL, (std::min)(static_cast<long long>(leftMs),
                                                                               static_cast<long long>(kIdleCaptureTimeoutMs))));
            }
            if (!paused && (PauseSignals::locked.load() || PauseSignals::displayOff.load())) {
                const PauseReason r = pauseReasonNow(CurrentInputDesktop());
                if (r != PauseReason::None) {
                    std::lock_guard<std::mutex> lock(pipelineMutex);
                    enterPause(r, false);
                } else {
                    PauseSignals::locked = false; // stale: WTS says unlocked
                }
            }
            if (paused) {
                // No capture while paused: this replaces AcquireNextFrame's
                // timeout as the loop's only wait, on the tray's wake event.
                capMs = UnixEpochMs();
                const InputDesktop desk = CurrentInputDesktop();
                const PauseReason want = pauseReasonNow(desk);
                const auto nowP = Clock::now();
                if (want == PauseReason::None) {
                    // One attempt per keepalive period, never a tight retry loop.
                    if (lastResumeTry == Clock::time_point{} ||
                        nowP - lastResumeTry >= std::chrono::milliseconds(kKeepaliveMs)) {
                        lastResumeTry = nowP;
                        std::lock_guard<std::mutex> lock(pipelineMutex);
                        if (buildPipeline(width, height, BuildReason::CaptureRebuild)) {
                            paused = false;
                            pauseFromSecureDesktop = false;
                            cursorPaused = false;
                            cursorForce = true;
                            haveGpuDesktopFrame = false;
                            waitFirstFrameSince = {};
                            firstFrameNudged = false;
                            firstFrameWaitLogged = false;
                            framePending = true; // encode the first capture at once
                            nextSlot = Clock::now();
                            encoder.RequestKeyFrame();
                            NotifyStatus();
                            Logf(ip, "resumed after %s: duplication rebuilt, keyframe requested\n",
                                 PauseReasonName(pauseReason));
                            lastResumeTry = {};
                            continue;
                        }
                        if (!resumeFailLogged) {
                            resumeFailLogged = true;
                            Logf(ip, "resume: duplication rebuild failed, still paused - retrying each second\n");
                        }
                    }
                } else if (want != pauseReason) {
                    std::lock_guard<std::mutex> lock(pipelineMutex);
                    renderPauseFrame(want);
                    Logf(ip, "pause reason now %s\n", PauseReasonName(want));
                }
                if (nowP - lastSend >= std::chrono::milliseconds(kKeepaliveMs) && pauseEncodesDue == 0)
                    pauseEncodesDue = 1; // keepalive: the overlay again
                {
                    std::lock_guard<std::mutex> lock(pipelineMutex);
                    if (pauseEncodesDue > 0 && !pauseNv12.empty()) {
                        --pauseEncodesDue;
                        encoded = encoder.EncodeNv12(pauseNv12.data(), pauseNv12.size());
                        ++cadenceEncCalls;
                    } else {
                        pauseEncodesDue = 0;
                        encoded = encoder.CollectPending();
                    }
                    cadenceAus += static_cast<uint32_t>(encoded.size());
                }
                if (encoded.empty() && pauseEncodesDue == 0) {
                    const auto untilKeepalive =
                        std::chrono::ceil<std::chrono::milliseconds>(lastSend + std::chrono::milliseconds(kKeepaliveMs) -
                                                                     Clock::now())
                            .count();
                    // Lock/unlock, display on/off and desktop switches (UAC
                    // secure desktop) all set the wake event; otherwise only
                    // the keepalive is due.
                    const DWORD waitMs = static_cast<DWORD>((std::max)(1LL, static_cast<long long>(untilKeepalive)));
                    WaitForSingleObject(PauseSignals::WakeEvent(), waitMs);
                }
            } else {
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
                    cap = dup.CaptureFrameNv12Gpu(gpuNv12, encoder.Width(), encoder.Height(), captureTimeoutMs);
                    if (cap.desktopChanged && !gpuNv12) {
                        // VP convert failed mid-stream - drop to CPU for this process.
                        Logf(ip, "GPU NV12 capture failed - falling back to CPU BGRA->NV12\n");
                        gpuNv12Path = false;
                        haveGpuDesktopFrame = false;
                        nv12.resize(static_cast<size_t>(dup.Width()) * dup.Height() * 3 / 2);
                        cap = dup.CaptureFrameNv12(nv12, captureTimeoutMs);
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
                    cap = dup.CaptureFrameNv12(nv12, captureTimeoutMs);
                }
                const bool changed = cap.desktopChanged;
                if (changed) {
                    ++cadencePresents;
                    framePending = true; // newest image is in nv12 / the GPU texture
                }
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
                    // Lock screen / UAC (secure desktop) or a dark display: pause
                    // instead of rebuilding into a desktop we cannot capture.
                    {
                        const InputDesktop desk = CurrentInputDesktop();
                        if (desk != InputDesktop::Default || PauseSignals::locked.load() ||
                            PauseSignals::displayOff.load()) {
                            enterPause(pauseReasonNow(desk) == PauseReason::None ? PauseReason::Paused
                                                                                   : pauseReasonNow(desk),
                                       desk != InputDesktop::Default);
                            haveGpuDesktopFrame = false;
                            continue;
                        }
                    }
                    haveGpuDesktopFrame = false;
                    waitFirstFrameSince = {};
                    firstFrameNudged = false;
                    firstFrameWaitLogged = false;
                    Logf(ip, "capture ACCESS_LOST - rebuilding pipeline (new D3D device)\n");
                    gpuNv12Path = false;
                    // ACCESS_LOST is usually a Display Settings Apply: let the
                    // backend persist the user's layout first; the rebuild never
                    // re-applies x/y or reshapes the head.
                    backend->OnLayoutMayHaveChanged();
                    if (!buildPipeline(width, height, BuildReason::CaptureRebuild)) {
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
                if (cap.pointerShapeChanged) {
                    PointerShapeBgra shape;
                    if (dup.GetPointerShapeBgra(shape)) {
                        pendingCursorImg = MakeCursorImgMessage(shape, dup.Width(), dup.Height());
                    }
                }
                // Positions come from the cursor thread (refresh rate, not
                // encode rate); a new shape makes it resend the position.
                if (!pendingCursorImg.empty())
                    cursorForce = true;

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
                    backend->OnLayoutMayHaveChanged();
                    haveGpuDesktopFrame = false;
                    gpuNv12Path = false;
                    waitFirstFrameSince = {};
                    firstFrameNudged = false;
                    firstFrameWaitLogged = false;
                    const bool swappedHello = (wantW == height && wantH == width);
                    uint32_t rebuildW = wantW;
                    uint32_t rebuildH = wantH;
                    if (backend->KeepsHelloLandscape()) {
                        // MTT: a portrait capture of the landscape hello is
                        // forced back to landscape; any other size is adopted.
                        if (swappedHello) {
                            rebuildW = width;
                            rebuildH = height;
                            if (backend->ForceMode(rebuildW, rebuildH, kFps))
                                Logf(ip, "capture was portrait swap of hello - forced MTT %ux%u landscape\n",
                                     rebuildW, rebuildH);
                            else
                                Logf(ip, "capture was portrait swap of hello - MTT mode force failed\n");
                        } else {
                            width = wantW;
                            height = wantH;
                        }
                    } else if (swappedHello) {
                        // Parsec (upstream): a rotation made in Windows is the
                        // user's decision and is adopted, not undone.
                        std::swap(width, height);
                    }
                    // Parsec, foreign size: encode what is captured but keep the
                    // panel size, so the watchdog below can put it back.
                    if (!buildPipeline(rebuildW, rebuildH, BuildReason::CaptureRebuild)) {
                        Logf(ip, "pipeline rebuild after resize failed, dropping the connection\n");
                        running = false;
                    } else {
                        width_ = dup.Width();
                        height_ = dup.Height();
                        input.SetMonitorRect(backend->CurrentRect());
                        continue; // capture a fresh frame on the new pipeline
                    }
                }

                auto now = std::chrono::steady_clock::now();

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
                           now - wrongSizeSince > std::chrono::milliseconds(kWrongSizeGraceMs) &&
                           backend->RebuildsOnForeignSize()) {
                    // Parsec (upstream): a remove + re-add is what gets the
                    // panel size back; re-applying the mode on the live monitor
                    // is refused (DISP_CHANGE_BADMODE) while the capture runs.
                    // Exactly one attempt: a rebuild resets every parsec
                    // monitor in turn, so retrying is how two senders end up
                    // trading rebuilds forever.
                    sizeRebuildDone = true;
                    Logf(ip, "monitor sits at %ux%u instead of %ux%u, rebuilding once\n", dup.Width(), dup.Height(),
                         width, height);
                    if (!buildPipeline(width, height))
                        Logf(ip, "rebuild for the panel size failed, keeping what we have\n");
                    else
                        continue; // nv12 still holds the pre-rebuild frame - capture a fresh one
                } else if (sizeRebuildDone && !sizeGiveUpLogged && backend->RebuildsOnForeignSize()) {
                    // parsec-vdd puts *one* custom resolution on all of its
                    // virtual monitors: whichever sender sets its panel size
                    // last drags every other monitor along. Said once per
                    // connection, then we stop touching the monitor.
                    sizeGiveUpLogged = true;
                    Logf(ip, "monitor stays at %ux%u (this iPad is %ux%u): parsec-vdd shares one custom resolution "
                             "across all its monitors, so the picture stays letterboxed here\n",
                         dup.Width(), dup.Height(), width, height);
                } else if (!sizeRebuildDone &&
                           now - wrongSizeSince > std::chrono::milliseconds(kWrongSizeGraceMs)) {
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

                // Keepalive re-encodes the last image (a tiny P-frame) so the
                // iPad's ~5s liveness watchdog (spec §5) never trips on an
                // otherwise idle desktop. GPU path only encodes once we have a
                // real present (CaptureFrameNv12Gpu returns null until then);
                // do not fall through to an empty CPU buffer.
                const bool keepaliveDue = now - lastSend >= std::chrono::milliseconds(kKeepaliveMs);

                // Encode only a new image (pending) or the keepalive, and only
                // once its slot opens (a little early is fine). Nothing new
                // means nothing encoded, apart from the single tail above. The
                // async encoder's held AU is collected below in between.
                const auto nowEnc = Clock::now();
                const bool slotOpen = nowEnc >= nextSlot - earlyWindow;
                const bool tailDue = tailPending && !framePending && nowEnc >= tailAt;
                const bool doEncode = running && slotOpen && (framePending || keepaliveDue || tailDue);
                if (doEncode) {
                    // A new image schedules one tail; the tail or keepalive does not.
                    tailPending = framePending;
                    framePending = false;
                    nextSlot += slot;
                    if (nowEnc > nextSlot + slot) // idle or stalled: re-anchor
                        nextSlot = nowEnc + slot;
                    tailAt = nextSlot + slot / 2;
                } else if (running) {
                    encoded = encoder.CollectPending();
                    cadenceAus += static_cast<uint32_t>(encoded.size());
                }

                if (doEncode) {
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
                            encoder.RequestKeyFrame(); // new encoder: start on an IDR
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
                    // pending frame: the limiter re-feeds the last captured image
                    // in the next slot, no second timer needed (upstream does
                    // the same with a dedicated 30ms replay timer, see #207).
                    //
                    // Only for the *first* drop of an episode. Re-arming on
                    // every failed attempt would keep a frame pending for as long
                    // as the link stays congested, burning an encode per
                    // WaitWritable timeout on frames nobody can receive. One
                    // prompt replay, then fall back to the keepalive until the
                    // socket drains.
                    if (!dropPending) {
                        framePending = true; // re-send the current image in the next slot
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

        cursorStop = true;
        cursorThread.join();
        if (usbWatch.joinable())
            usbWatch.join();
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
        // stopRequested_ => tray Disconnect / Exit / Stop (user-initiated).
        // Otherwise socket drop / transport flap: MTT keeps its head for a
        // grace window; Parsec keeps its monitor for the sender's lifetime.
        backend->OnSessionEnd(lastClient, stopRequested_.load());
        width_ = 0;
        height_ = 0;
        SetTransport(Transport::None);
        Logf(ip, "disconnected%s\n",
             stopRequested_ ? "" : (movedToUsb ? ", reconnecting over USB" : ", reconnecting"));
    }

    if (ipLock != nullptr)
        CloseHandle(ipLock);
    SetState(State::Idle);
}

} // namespace od

