#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace od {

// Wraps the parsec-vdd driver (third_party/parsec-vdd/parsec-vdd.h) to
// provide/manage exactly one virtual monitor sized to the iPad panel. Several
// instances can run side by side (one per iPad): each claims the monitor its
// own VddAddDisplay produced and keeps its own saved position, keyed by the
// identity passed to SetIdentity.
//
// Needs admin rights (custom-resolution registry lives under HKLM, verified
// non-writable without elevation — see CMakeLists' /MANIFESTUAC flag).
class VirtualDisplay {
public:
    VirtualDisplay();
    ~VirtualDisplay();

    VirtualDisplay(const VirtualDisplay&) = delete;
    VirtualDisplay& operator=(const VirtualDisplay&) = delete;

    // Opens the parsec-vdd device and starts the <100ms keepalive ping the
    // driver requires (its own doc comment). Returns false if the driver
    // isn't installed/accessible.
    bool Open();
    void Close();

    // Names this display's owner (the iPad's address) so its desktop position
    // is saved per iPad rather than shared. Call before EnsureResolution.
    void SetIdentity(const std::string& id);

    // Registers width x height @ hz as the custom-resolution slot, then
    // (re)adds the virtual display and attaches/resizes it to exactly that
    // mode. Safe to call again with new dimensions (rotation) — removes and
    // re-adds the display so the driver re-reads the registry.
    bool EnsureResolution(uint32_t width, uint32_t height, uint32_t hz = 60);

    // Rect of the virtual monitor within the Windows virtual desktop
    // (SM_XVIRTUALSCREEN-relative), valid after a successful EnsureResolution().
    // Thread-safe: the keepalive poll updates it when the user drags the
    // monitor, so callers (e.g. input mapping) can pick up live moves.
    RECT MonitorRect() const;

    // Re-reads the rect from Windows. The keepalive poll only refreshes it when
    // the monitor *moves*, so a rotation in place leaves the cached rect at the
    // old geometry — call this after a Windows-side rotation before handing the
    // rect to the input mapping.
    bool QueryMonitorRect();

    // GDI device name (e.g. L"\\.\DISPLAY3") of the virtual monitor, needed
    // by DesktopDuplication to pick the matching IDXGIOutput.
    const std::wstring& DeviceName() const { return deviceName_; }

    bool IsOpen() const { return device_ != nullptr; }

    // One-off maintenance: remove phantom (non-present) parsec virtual monitors
    // left behind by earlier runs, returning the count. Explicitly NOT called
    // automatically — device removal is too invasive for the normal path (see
    // the ROADMAP). Needs admin.
    static int CleanupGhostMonitors();

    // Unplugs the virtual display at `index` (0..VDD_MAX_DISPLAYS-1) through a
    // fresh driver handle. Recovery for a sender that was killed instead of
    // shut down: the driver keeps its display plugged in — attached to the
    // desktop, not just a phantom devnode — until someone removes it by index.
    // Indexes are driver-global, so this can unplug a *running* sender's
    // monitor too; only run it when the sender is stopped.
    static bool RemoveDisplayIndex(int index);

    // Registers a custom resolution (and its rotation) as a parsec-vdd mode so
    // the virtual monitor can use the iPad's native size. Writing needs admin;
    // used by the `--register-resolution` one-off and by the self-elevate path.
    static bool RegisterResolutions(uint32_t width, uint32_t height);

private:
    bool SelfElevateRegister(uint32_t width, uint32_t height);

    // Claims the monitor that attached since `attachedBefore` was snapshotted
    // (i.e. the one our VddAddDisplay produced) and places/sizes it.
    bool FindMonitorGeometry(const std::set<std::wstring>& attachedBefore);
    void KeepAliveLoop();
    std::wstring PositionValueName(const wchar_t* base) const;

    // Persist the monitor's desktop position ourselves (HKCU): the parsec-vdd
    // monitor gets a fresh identity/UID on every add, so Windows can't
    // remember its arrangement across app restarts — it would snap back to a
    // default every launch. We save the position and reapply it, and poll for
    // the user dragging it in Display Settings to keep the saved value current.
    bool LoadSavedPosition(int& x, int& y) const;
    void SavePosition(int x, int y) const;
    void PollPosition(POINT& lastKnown);

    HANDLE device_ = nullptr;
    int displayIndex_ = -1;
    std::thread keepAliveThread_;
    std::atomic<bool> keepAliveRunning_{false};

    // Serializes every parsec-vdd driver call on device_ (the VddUpdate keepalive
    // vs. VddAddDisplay/VddRemoveDisplay on rotation). Two DeviceIoControls in
    // flight on the same handle race the driver's monitor-topology state — that
    // crashes DWM / the Display Settings dialog and flaps the monitor out from
    // under the live capture. Locked per call only (never across the settle
    // sleeps), so the keepalive is never starved past the driver's ~10s watchdog.
    std::mutex vddMutex_;

    std::string identity_; // owner id (iPad address): log tag and saved-position key

    uint32_t targetWidth_ = 0;
    uint32_t targetHeight_ = 0;
    uint32_t targetHz_ = 60;

    mutable std::mutex stateMutex_; // guards deviceName_ (written by main, read by keepalive poll)
    RECT monitorRect_{};
    std::wstring deviceName_;
};

} // namespace od
