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
class InputInjector {
public:
    // Rect of the virtual monitor within the Windows virtual desktop
    // (VirtualDisplay::MonitorRect()). Must be set before touches arrive;
    // update again on rotation (new hello -> new rect).
    void SetMonitorRect(const RECT& rect) { monitorRect_ = rect; }

    void HandleTouch(const TouchMsg& touch);
    void HandleScroll(const ScrollMsg& scroll);

private:
    RECT monitorRect_{};
    bool isDown_ = false;
};

} // namespace od
