#pragma once

#include <windows.h>

#include "net/Protocol.h"

namespace od {

// Maps receiver touch/scroll control messages to Win32 SendInput mouse
// events (spec §7d). Touch coordinates are normalized [0,1], origin
// top-left, relative to the video image — mapped to the virtual monitor's
// rect within the Windows virtual desktop, then to SendInput's 0..65535
// absolute space over the *entire* virtual desktop (SM_XVIRTUALSCREEN/
// SM_CXVIRTUALSCREEN — the classic multi-monitor absolute-coordinate pitfall).
// Apple Pencil takes a different route: a synthetic pen device
// (CreateSyntheticPointerDevice, Windows 10 1809+) fed with POINTER_PEN_INFO,
// which is what carries pressure, tilt and hover. Windows synthesizes the
// mouse messages for non-pointer-aware apps from it, so clicking UI still
// works; WinTab-only apps (older Photoshop setups) do not see it.
class InputInjector {
public:
    ~InputInjector();

    InputInjector() = default;
    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    // Rect of the virtual monitor within the Windows virtual desktop
    // (VirtualDisplay::MonitorRect()). Must be set before touches arrive;
    // update again on rotation (new hello -> new rect).
    void SetMonitorRect(const RECT& rect) { monitorRect_ = rect; }

    void HandleTouch(const TouchMsg& touch);
    void HandleScroll(const ScrollMsg& scroll);
    void HandlePencil(const PencilMsg& pencil);
    void HandleProximity(const ProximityMsg& proximity);

private:
    POINT ScreenPoint(double nx, double ny) const;
    bool EnsurePenDevice();
    void InjectPen(UINT32 flags, POINT pt, double pressure, double azimuth, double altitude);
    void ReleasePenIfDown(POINT pt);

    RECT monitorRect_{};
    bool isDown_ = false;

    HSYNTHETICPOINTERDEVICE penDevice_ = nullptr;
    bool penDeviceFailed_ = false; // creation failed once -> stop retrying per event
    bool injectFailed_ = false;    // log the first injection failure only
    bool penDown_ = false;
    bool penInRange_ = false;
};

} // namespace od
