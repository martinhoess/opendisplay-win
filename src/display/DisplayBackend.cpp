#include "display/DisplayBackend.h"

#include "app/Log.h"
#include "display/ExistingMonitor.h"
#include "display/MttVddSettings.h"
#include "display/VirtualDisplay.h"

#include "parsec-vdd.h"

#include <climits>
#include <mutex>

namespace od {

const char* DisplayBackendName(DisplayBackendKind kind)
{
    return kind == DisplayBackendKind::Parsec ? "Parsec VDD" : "MTT VDD";
}

bool IsMttDriverInstalled()
{
    std::wstring id;
    return FindMttAdapterInstanceId(id);
}

bool IsParsecDriverInstalled()
{
    return parsec_vdd::QueryDeviceStatus(&parsec_vdd::VDD_CLASS_GUID, parsec_vdd::VDD_HARDWARE_ID) ==
           parsec_vdd::DEVICE_OK;
}

DisplayBackendSelection SelectDisplayBackend(const std::string& configValue)
{
    DisplayBackendSelection sel;
    if (configValue == "mtt" || configValue == "parsec") {
        sel.automatic = false;
        sel.kind = configValue == "mtt" ? DisplayBackendKind::Mtt : DisplayBackendKind::Parsec;
        sel.driverInstalled = sel.kind == DisplayBackendKind::Mtt ? IsMttDriverInstalled() : IsParsecDriverInstalled();
        if (!sel.driverInstalled)
            sel.problem = std::string(DisplayBackendName(sel.kind)) + " is selected but not installed";
        return sel;
    }
    sel.automatic = true;
    if (IsMttDriverInstalled()) {
        sel.kind = DisplayBackendKind::Mtt;
        sel.driverInstalled = true;
    } else if (IsParsecDriverInstalled()) {
        sel.kind = DisplayBackendKind::Parsec;
        sel.driverInstalled = true;
    } else {
        sel.kind = DisplayBackendKind::Mtt;
        sel.driverInstalled = false;
        sel.problem = "no virtual display driver (install MTT VDD or Parsec VDD)";
    }
    return sel;
}

uint32_t DisplayBackend::RefreshHz() const
{
    const std::wstring name = DeviceName();
    if (name.empty())
        return 60;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1)
        return 60;
    return dm.dmDisplayFrequency;
}

namespace {

// ---- MTT ----------------------------------------------------------------------
// The MTT head is shared by every sender (one sticky head); per-iPad EDID,
// attach/verify and the saved layout live in MttVddSettings. This is the code
// that used to sit inline in SenderApp::buildPipeline, unchanged in behaviour.
class MttBackend final : public DisplayBackend {
public:
    MttBackend(std::string tag, uint32_t fps, const std::atomic<bool>* cancel)
        : tag_(std::move(tag)), fps_(fps), cancel_(cancel)
    {
    }

    DisplayBackendKind Kind() const override { return DisplayBackendKind::Mtt; }

    void BeginConnection(uint32_t helloW, uint32_t helloH) override
    {
        helloW_ = helloW;
        helloH_ = helloH;
    }

    bool Prepare(uint32_t width, uint32_t height, BuildReason /*reason*/) override
    {
        const std::string& ip = tag_;
        const uint32_t kFps = fps_;

        // Per-iPad EDID BEFORE the head attaches: modes generated from this
        // hello size (first build of a connection = hello WxH). Same iPad
        // again = no-op; a size change self-elevates once (tray notice first)
        // and reloads MttVDD. UAC decline keeps the installed EDID and still
        // connects. Never re-run on capture rebuilds.
        if (!modesEnsured_) {
            const uint32_t edidW = helloW_ ? helloW_ : width;
            const uint32_t edidH = helloH_ ? helloH_ : height;
            (void)EnsureMttResolutionsForHello(edidW, edidH, ip, cancel_);
            modesEnsured_ = true; // even on UAC decline: one prompt per connection
        }

        // Re-attach MTT head if a prior last-client teardown dropped it.
        Logf(ip, "mtt: ensuring VDD for hello\n");
        (void)EnsureMttVddAttached(ip, cancel_);
        ExistingMonitor mon{};
        using_ = FindMttVirtualMonitor(mon);
        if (using_ && !helloModeForced_) {
            // First build of the connection: prove the driver actually offers
            // this iPad's modes before applying / saving any of them. (A
            // reload that did not happen leaves MttVDD on its stock 800x600+
            // list; that layout must never reach mtt_display.json.)
            (void)VerifyMttModesOffered(ip, 6000);
            using_ = FindMttVirtualMonitor(mon); // a restart can renumber the DISPLAYn name
        }
        if (!using_) {
            Logf(ip, "MTT virtual monitor not found (is MTT VDD installed and enabled?)\n");
            return false;
        }
        Logf(ip, "using MTT virtual monitor %ls (%ls)\n", mon.deviceName.c_str(), mon.deviceString.c_str());
        // First hello of a connection: restore the saved mode (the user's
        // Display Settings pick) only if this iPad's EDID offers it;
        // otherwise the preferred mode. Later rebuilds honor Keep and only
        // fix orientation / refresh.
        if (!helloModeForced_) {
            uint32_t savedW = 0, savedH = 0, savedHz = 0;
            if (QueryMttSavedMode(savedW, savedH, savedHz)) {
                const uint32_t applyHz = savedHz ? savedHz : kFps;
                if (!EnsureMonitorMode(mon.deviceName, savedW, savedH, applyHz)) {
                    Logf(ip, "mtt: restoring saved mode %ux%u@%u failed (keeping current)\n", savedW, savedH,
                         applyHz);
                    if (EnsureMonitorLandscapeOrientation(mon.deviceName))
                        Logf(ip, "MTT orientation ensured landscape (resolution unchanged)\n");
                    if (!EnsureMonitorRefresh(mon.deviceName, applyHz))
                        Logf(ip, "MTT refresh %u Hz request failed (keeping current)\n", applyHz);
                } else {
                    Logf(ip, "mtt: restoring saved mode %ux%u@%u\n", savedW, savedH, applyHz);
                }
            } else {
                // No saved mode, or it is not offered by this iPad's EDID:
                // use the preferred (landscape hello-native) mode.
                const MttMode preferred = CurrentMttModes().front();
                if (!EnsureMonitorMode(mon.deviceName, preferred.width, preferred.height, kFps)) {
                    Logf(ip, "mtt: no usable saved mode - preferred %ux%u failed "
                             "(keeping current)\n",
                         preferred.width, preferred.height);
                    if (!EnsureMonitorRefresh(mon.deviceName, kFps))
                        Logf(ip, "MTT refresh %u Hz request failed (keeping current)\n", kFps);
                    else
                        Logf(ip, "MTT refresh set to %u Hz\n", kFps);
                } else {
                    Logf(ip, "mtt: no usable saved mode - using preferred %ux%u\n", preferred.width,
                         preferred.height);
                }
            }
            helloModeForced_ = true;
        } else {
            if (EnsureMonitorLandscapeOrientation(mon.deviceName))
                Logf(ip, "MTT orientation ensured landscape (resolution unchanged)\n");
            else
                Logf(ip, "MTT landscape orientation request failed (keeping current)\n");
            if (!EnsureMonitorRefresh(mon.deviceName, kFps))
                Logf(ip, "MTT refresh %u Hz request failed (keeping current)\n", kFps);
        }
        RECT rect = mon.rect;
        (void)GetMonitorRectByDeviceName(mon.deviceName, rect); // keep the Find rect if this fails
        {
            std::lock_guard<std::mutex> lock(mu_);
            deviceName_ = mon.deviceName;
            rect_ = rect;
        }
        return true;
    }

    std::wstring DeviceName() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return deviceName_;
    }

    RECT CurrentRect() override
    {
        std::wstring name;
        {
            std::lock_guard<std::mutex> lock(mu_);
            name = deviceName_;
        }
        RECT r{};
        const bool live = !name.empty() && GetMonitorRectByDeviceName(name, r);
        std::lock_guard<std::mutex> lock(mu_);
        if (live)
            rect_ = r;
        return rect_;
    }

    bool KeepsHelloLandscape() const override { return true; }

    bool ForceMode(uint32_t width, uint32_t height, uint32_t hz) override
    {
        const std::wstring name = DeviceName();
        return !name.empty() && EnsureMonitorMode(name, width, height, hz);
    }

    bool RebuildsOnForeignSize() const override { return false; }

    void OnStreaming() override
    {
        if (using_)
            (void)SaveMttDisplayTopology(tag_);
    }

    void OnLayoutMayHaveChanged() override
    {
        // ACCESS_LOST is usually a Display Settings Apply. Persist the user's
        // new layout first (only if MTT is attached+active and this was not
        // our own apply); the rebuild never re-applies x/y.
        if (using_)
            (void)SaveMttDisplayTopologyIfUserChange(tag_);
    }

    void OnSessionEnd(bool lastClient, bool userInitiated) override
    {
        if (!lastClient || !using_)
            return;
        // userInitiated => tray Disconnect / Exit / Stop. Otherwise socket
        // drop / transport flap: grace window, sticky VDD.
        const MttTeardownReason reason = userInitiated ? MttTeardownReason::UserInitiated : MttTeardownReason::LinkLoss;
        RequestMttVddTeardown(tag_, reason);
        if (reason == MttTeardownReason::UserInitiated)
            using_ = false;
        modesEnsured_ = false;
        helloModeForced_ = false;
    }

    void OnPipelineFailed(bool lastClient) override
    {
        if (!lastClient || !using_)
            return;
        // Pipeline failed but session may reconnect - keep VDD sticky.
        RequestMttVddTeardown(tag_, MttTeardownReason::LinkLoss);
        modesEnsured_ = false;
        helloModeForced_ = false;
    }

private:
    std::string tag_;
    uint32_t fps_;
    const std::atomic<bool>* cancel_;
    uint32_t helloW_ = 0, helloH_ = 0; // raw hello size: the per-iPad EDID source
    bool using_ = false;
    bool modesEnsured_ = false;    // once per connection; Ensure is idempotent too
    bool helloModeForced_ = false; // apply saved/preferred mode once; later honor Keep
    mutable std::mutex mu_; // deviceName_ / rect_: read by reader + cursor threads
    std::wstring deviceName_;
    RECT rect_{};
};

// ---- Parsec -------------------------------------------------------------------
// Upstream's backend, restored from origin/master (06af3a9): VirtualDisplay
// owns one parsec-vdd monitor per sender (so several iPads run side by side)
// for the sender's lifetime, keeps the driver alive, registers the panel size
// as a custom mode (self-elevating once) and saves its position per iPad.
class ParsecBackend final : public DisplayBackend {
public:
    ParsecBackend(std::string tag, uint32_t fps) : tag_(std::move(tag)), fps_(fps)
    {
        // Keeps this sender's monitor position separate from the other iPads' -
        // several senders share one HKCU key.
        vdisp_.SetIdentity(tag_);
    }

    DisplayBackendKind Kind() const override { return DisplayBackendKind::Parsec; }

    void BeginConnection(uint32_t, uint32_t) override {}

    bool Prepare(uint32_t width, uint32_t height, BuildReason reason) override
    {
        if (!vdisp_.IsOpen() && !vdisp_.Open()) {
            Logf(tag_, "VirtualDisplay::Open failed (parsec-vdd driver missing/inaccessible?)\n");
            return false;
        }
        // Capture-driven rebuild (ACCESS_LOST, a rotation or foreign size
        // seen in the capture): the monitor is already there - re-read where
        // it is instead of re-adding it (upstream only reconfigured the
        // encoder in that case). The rect is cached and only refreshed on
        // moves, so a rotation in place needs the explicit re-query.
        // If the driver dropped the monitor (standby/resume), fall through and
        // add it again.
        if (reason == BuildReason::CaptureRebuild && !vdisp_.DeviceName().empty() && vdisp_.QueryMonitorRect()) {
            publishName();
            return true;
        }
        if (!vdisp_.EnsureResolution(width, height, fps_)) {
            Logf(tag_, "VirtualDisplay::EnsureResolution failed (run as Administrator?)\n");
            return false;
        }
        publishName();
        return true;
    }

    std::wstring DeviceName() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return deviceName_;
    }

    RECT CurrentRect() override { return vdisp_.MonitorRect(); }

    bool KeepsHelloLandscape() const override { return false; }
    bool ForceMode(uint32_t, uint32_t, uint32_t) override { return false; }
    bool RebuildsOnForeignSize() const override { return true; }

private:
    // VirtualDisplay::DeviceName() is a plain reference meant for the pipeline
    // thread; keep a locked copy for the reader / cursor threads.
    void publishName()
    {
        std::lock_guard<std::mutex> lock(mu_);
        deviceName_ = vdisp_.DeviceName();
    }

    std::string tag_;
    uint32_t fps_;
    VirtualDisplay vdisp_;
    mutable std::mutex mu_;
    std::wstring deviceName_;
};

} // namespace

std::unique_ptr<DisplayBackend> MakeDisplayBackend(DisplayBackendKind kind, const std::string& logTag, uint32_t fps,
                                                   const std::atomic<bool>* cancel)
{
    if (kind == DisplayBackendKind::Parsec)
        return std::make_unique<ParsecBackend>(logTag, fps);
    return std::make_unique<MttBackend>(logTag, fps, cancel);
}

} // namespace od
