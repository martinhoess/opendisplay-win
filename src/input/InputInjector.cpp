#include "input/InputInjector.h"

namespace od {

namespace {

constexpr double kPixelsPerWheelNotch = 100.0; // heuristic; not specified by the wire protocol

LONG NormalizeToVirtualDesktop(LONG screenCoord, int origin, int extent)
{
    if (extent <= 0)
        return 0;
    return static_cast<LONG>((static_cast<double>(screenCoord - origin) * 65535.0) / extent);
}

void SendMouseInput(DWORD flags, LONG dx = 0, LONG dy = 0, LONG mouseData = 0)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.mouseData = mouseData;
    input.mi.dwFlags = flags;
    SendInput(1, &input, sizeof(INPUT));
}

} // namespace

void InputInjector::HandleTouch(const TouchMsg& touch)
{
    LONG screenX = monitorRect_.left + static_cast<LONG>(touch.x * (monitorRect_.right - monitorRect_.left));
    LONG screenY = monitorRect_.top + static_cast<LONG>(touch.y * (monitorRect_.bottom - monitorRect_.top));

    LONG normX = NormalizeToVirtualDesktop(screenX, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN));
    LONG normY = NormalizeToVirtualDesktop(screenY, GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));

    DWORD moveFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    switch (touch.phase) {
        case TouchPhase::Began:
            SendMouseInput(moveFlags, normX, normY);
            SendMouseInput(MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, normX, normY);
            isDown_ = true;
            break;
        case TouchPhase::Moved:
            SendMouseInput(moveFlags, normX, normY);
            break;
        case TouchPhase::Ended:
        case TouchPhase::Cancelled:
            SendMouseInput(moveFlags, normX, normY);
            if (isDown_)
                SendMouseInput(MOUSEEVENTF_LEFTUP | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, normX, normY);
            isDown_ = false;
            break;
        default:
            break;
    }
}

void InputInjector::HandleScroll(const ScrollMsg& scroll)
{
    if (scroll.dy != 0.0) {
        LONG delta = static_cast<LONG>((scroll.dy / kPixelsPerWheelNotch) * WHEEL_DELTA);
        if (delta != 0)
            SendMouseInput(MOUSEEVENTF_WHEEL, 0, 0, delta);
    }
    if (scroll.dx != 0.0) {
        LONG delta = static_cast<LONG>((scroll.dx / kPixelsPerWheelNotch) * WHEEL_DELTA);
        if (delta != 0)
            SendMouseInput(MOUSEEVENTF_HWHEEL, 0, 0, delta);
    }
}

} // namespace od
