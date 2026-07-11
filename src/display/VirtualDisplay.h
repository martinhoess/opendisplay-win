#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace od {

// Wraps the parsec-vdd driver (third_party/parsec-vdd/parsec-vdd.h) to
// provide/manage exactly one virtual monitor sized to the iPad panel.
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

    // Registers width x height @ hz as the custom-resolution slot, then
    // (re)adds the virtual display and attaches/resizes it to exactly that
    // mode. Safe to call again with new dimensions (rotation) — removes and
    // re-adds the display so the driver re-reads the registry.
    bool EnsureResolution(uint32_t width, uint32_t height, uint32_t hz = 60);

    // Rect of the virtual monitor within the Windows virtual desktop
    // (SM_XVIRTUALSCREEN-relative), valid after a successful EnsureResolution().
    RECT MonitorRect() const { return monitorRect_; }

    // GDI device name (e.g. L"\\.\DISPLAY3") of the virtual monitor, needed
    // by DesktopDuplication to pick the matching IDXGIOutput.
    const std::wstring& DeviceName() const { return deviceName_; }

    bool IsOpen() const { return device_ != nullptr; }

private:
    bool WriteCustomResolutionRegistry(uint32_t width, uint32_t height, uint32_t hz);
    bool FindMonitorGeometry();
    bool QueryMonitorRect();
    void KeepAliveLoop();

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

    uint32_t targetWidth_ = 0;
    uint32_t targetHeight_ = 0;
    uint32_t targetHz_ = 60;

    mutable std::mutex stateMutex_; // guards deviceName_ (written by main, read by keepalive poll)
    RECT monitorRect_{};
    std::wstring deviceName_;
};

} // namespace od
