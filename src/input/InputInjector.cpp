#include "input/InputInjector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace od {

namespace {

constexpr double kPixelsPerWheelNotch = 100.0; // heuristic; not specified by the wire protocol
constexpr UINT32 kPenPointerId = 1;
constexpr double kMaxPenPressure = 1024.0; // POINTER_PEN_INFO.pressure range
constexpr double kRadToDeg = 57.295779513082320876798; // 180/pi

// UIKit's spherical pen angles -> the tilt pair Windows wants (degrees,
// -90..90), using the W3C Pointer Events conversion. The macOS injector's
// formula is deliberately *not* reused: CGEvent tilt fields are normalized
// to -1..1, POINTER_PEN_INFO is in degrees.
void DeriveTilt(double azimuth, double altitude, INT32& tiltX, INT32& tiltY)
{
    double sinAlt = std::sin(altitude);
    double cosAlt = std::cos(altitude);
    double x = std::atan2(cosAlt * std::cos(azimuth), sinAlt) * kRadToDeg;
    double y = std::atan2(cosAlt * std::sin(azimuth), sinAlt) * kRadToDeg;
    tiltX = static_cast<INT32>(std::lround(std::clamp(x, -90.0, 90.0)));
    tiltY = static_cast<INT32>(std::lround(std::clamp(y, -90.0, 90.0)));
}

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

InputInjector::~InputInjector()
{
    if (penDevice_ != nullptr)
        DestroySyntheticPointerDevice(penDevice_);
}

POINT InputInjector::ScreenPoint(double nx, double ny) const
{
    // Pen injection takes physical pixels relative to the *top-left of the
    // virtual screen*, not SendInput's 0..65535 space and not raw desktop
    // coordinates: MonitorRect() is GetMonitorInfo's rcMonitor, so a monitor
    // left of or above the primary is negative there, and Windows drops such
    // frames without an error (InjectSyntheticPointerInput still returns TRUE).
    // Hence the same origin shift HandleTouch does inside its normalization.
    POINT pt;
    pt.x = monitorRect_.left + std::lround(nx * (monitorRect_.right - monitorRect_.left)) -
           GetSystemMetrics(SM_XVIRTUALSCREEN);
    pt.y = monitorRect_.top + std::lround(ny * (monitorRect_.bottom - monitorRect_.top)) -
           GetSystemMetrics(SM_YVIRTUALSCREEN);
    return pt;
}

bool InputInjector::EnsurePenDevice()
{
    if (penDevice_ != nullptr)
        return true;
    if (penDeviceFailed_)
        return false;

    penDevice_ = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
    if (penDevice_ == nullptr) {
        // Pre-1809 Windows, or the slot is taken. Give up for this session
        // rather than hammering the API once per pen sample; finger touch is
        // unaffected and the pen still moves the cursor as plain touch.
        fprintf(stderr, "CreateSyntheticPointerDevice(PT_PEN) failed: %lu\n", GetLastError());
        penDeviceFailed_ = true;
        return false;
    }
    return true;
}

void InputInjector::InjectPen(UINT32 flags, POINT pt, double pressure, double azimuth, double altitude)
{
    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    info.penInfo.pointerInfo.pointerType = PT_PEN;
    info.penInfo.pointerInfo.pointerId = kPenPointerId;
    info.penInfo.pointerInfo.ptPixelLocation = pt;
    info.penInfo.pointerInfo.pointerFlags = flags;
    info.penInfo.penFlags = PEN_FLAG_NONE;
    info.penInfo.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    info.penInfo.pressure =
        static_cast<UINT32>(std::lround(std::clamp(pressure, 0.0, 1.0) * kMaxPenPressure));
    DeriveTilt(azimuth, altitude, info.penInfo.tiltX, info.penInfo.tiltY);

    if (!InjectSyntheticPointerInput(penDevice_, &info, 1) && !injectFailed_) {
        injectFailed_ = true; // one line, not one per sample
        fprintf(stderr, "InjectSyntheticPointerInput failed: %lu (flags=0x%08X, pt=%ld,%ld)\n",
                GetLastError(), flags, pt.x, pt.y);
    }
}

void InputInjector::ReleasePenIfDown(POINT pt)
{
    if (!penDown_)
        return;
    InjectPen(POINTER_FLAG_INRANGE | POINTER_FLAG_UP, pt, 0.0, 0.0, kPencilAltitudeUpright);
    penDown_ = false;
}

void InputInjector::HandlePencil(const PencilMsg& pencil)
{
    if (!EnsurePenDevice())
        return;

    const POINT pt = ScreenPoint(pencil.x, pencil.y);

    switch (pencil.phase) {
        case PencilPhase::Down:
            InjectPen(POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN, pt,
                      pencil.pressure, pencil.azimuth, pencil.altitude);
            penDown_ = true;
            penInRange_ = true;
            break;
        case PencilPhase::Move:
            if (penDown_) {
                InjectPen(POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE, pt,
                          pencil.pressure, pencil.azimuth, pencil.altitude);
            } else {
                InjectPen(POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE, pt, 0.0, pencil.azimuth,
                          pencil.altitude);
            }
            penInRange_ = true;
            break;
        case PencilPhase::Up:
            ReleasePenIfDown(pt);
            break;
        case PencilPhase::Hover:
            // A stroke that ran off the panel edge comes back as hover while we
            // still hold the pen down — release it first, or the button stays
            // stuck (same recovery as the macOS injector).
            ReleasePenIfDown(pt);
            InjectPen(POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE, pt, 0.0, pencil.azimuth,
                      pencil.altitude);
            penInRange_ = true;
            break;
        default:
            break;
    }
}

void InputInjector::HandleProximity(const ProximityMsg& proximity)
{
    if (!EnsurePenDevice())
        return;
    if (proximity.entering == penInRange_)
        return;

    const POINT pt = ScreenPoint(proximity.x, proximity.y);

    if (proximity.entering) {
        InjectPen(POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE, pt, 0.0, 0.0, kPencilAltitudeUpright);
    } else {
        ReleasePenIfDown(pt);
        // No INRANGE: that is what tells Windows the pen left hover range.
        InjectPen(POINTER_FLAG_UPDATE, pt, 0.0, 0.0, kPencilAltitudeUpright);
    }
    penInRange_ = proximity.entering;
}

} // namespace od
