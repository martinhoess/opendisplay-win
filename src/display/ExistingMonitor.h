#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace od {

// Locates an already-attached virtual / physical monitor without creating one.
// Used to point DesktopDuplication at the Intel-pinned MTT Virtual Display
// Driver head instead of creating a Parsec VDD monitor.

struct ExistingMonitor {
    std::wstring deviceName; // e.g. L"\\.\DISPLAY25"
    std::wstring deviceString;
    std::wstring deviceId;
    RECT rect{};
};

// Prefer MTT Virtual Display Driver ("MTT1337" / "VDD by MTT"). Returns false
// if none is currently attached to the desktop.
bool FindMttVirtualMonitor(ExistingMonitor& out);

// Same lookup; when requireAttached is false also returns a detached MTT head
// (present in EnumDisplayDevices but not on the desktop) so we can re-attach.
bool FindMttVirtualMonitorDevice(ExistingMonitor& out, bool requireAttached);

// Attach a (possibly detached) GDI device with width x height @ hz.
// When hasPosition is true, place at (posX, posY); otherwise use a first-time
// default to the right of the current virtual desktop (not a hardcoded "below
// primary"). Prefer RestoreMttDisplayTopology after attach when a saved
// arrangement exists.
bool AttachMonitorToDesktop(const std::wstring& deviceName, uint32_t width, uint32_t height,
                            uint32_t hz, bool hasPosition = false, int posX = 0, int posY = 0);

// Move an attached monitor without changing its mode. Absolute virtual-desktop
// coordinates (same space as DEVMODE.dmPosition / MONITORINFO.rcMonitor).
bool SetMonitorDesktopPosition(const std::wstring& deviceName, int posX, int posY);

// Refresh rect for a known GDI device name.
bool GetMonitorRectByDeviceName(const std::wstring& deviceName, RECT& out);

// Best-effort: keep current resolution, set refresh to fps.
bool EnsureMonitorRefresh(const std::wstring& deviceName, uint32_t fps);

// Best-effort: set resolution + DMDO_DEFAULT (landscape) so DXGI capture
// matches hello WxH. Used on first hello / portrait-swap recovery only.
bool EnsureMonitorMode(const std::wstring& deviceName, uint32_t width, uint32_t height,
                       uint32_t fps);

// Leave resolution alone; only set DMDO_DEFAULT if orientation is wrong.
bool EnsureMonitorLandscapeOrientation(const std::wstring& deviceName);

// Best-effort: unplug a GDI device from the desktop (zeroed mode + apply).
// Used when MTT settings count=0 still leaves a head attached after reload.
bool DetachMonitorFromDesktop(const std::wstring& deviceName);

// Self-apply guard. Every display change *we* issue (CDS / SetDisplayConfig)
// stamps a tick so topology observers (ACCESS_LOST, WM_DISPLAYCHANGE, the
// streaming poll) can tell our own applies from a layout change the user made
// in Display Settings. Generation increments on every stamp.
void NoteSelfDisplayChange();
bool RecentSelfDisplayChange(uint32_t withinMs);
uint64_t SelfDisplayChangeGeneration();

} // namespace od
