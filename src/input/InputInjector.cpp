#include "input/InputInjector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace od {

namespace {

constexpr double kPixelsPerWheelNotch = 100.0; // heuristic; not specified by the wire protocol
constexpr UINT32 kPenPointerId = 1;
constexpr double kMaxPenPressure = 1024.0; // POINTER_PEN_INFO.pressure range
constexpr double kRadToDeg = 57.295779513082320876798; // 180/pi
constexpr ULONG kMaxTouchContacts = 10;
constexpr LONG kTouchContactHalfPx = 10; // default finger contact area (protocol has no size)
constexpr UINT32 kMaxTouchPressure = 1024;

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

void FillTouchInfo(POINTER_TYPE_INFO& info, UINT32 pointerId, POINT pt, UINT32 pointerFlags)
{
    info = {};
    info.type = PT_TOUCH;
    info.touchInfo.pointerInfo.pointerType = PT_TOUCH;
    info.touchInfo.pointerInfo.pointerId = pointerId;
    info.touchInfo.pointerInfo.ptPixelLocation = pt;
    info.touchInfo.pointerInfo.pointerFlags = pointerFlags;
    info.touchInfo.touchFlags = TOUCH_FLAG_NONE;
    // Protocol has no contact size; a small default area keeps apps that read
    // rcContact happy and matches typical finger injection samples.
    info.touchInfo.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
    info.touchInfo.rcContact.left = pt.x - kTouchContactHalfPx;
    info.touchInfo.rcContact.top = pt.y - kTouchContactHalfPx;
    info.touchInfo.rcContact.right = pt.x + kTouchContactHalfPx;
    info.touchInfo.rcContact.bottom = pt.y + kTouchContactHalfPx;
    info.touchInfo.pressure = kMaxTouchPressure;
}

} // namespace

bool InputInjector::EnsureTouchDevice()
{
    if (touchDevice_ != nullptr)
        return true;
    if (touchDeviceFailed_)
        return false;

    touchDevice_ = CreateSyntheticPointerDevice(PT_TOUCH, kMaxTouchContacts, POINTER_FEEDBACK_DEFAULT);
    if (touchDevice_ == nullptr) {
        fprintf(stderr, "CreateSyntheticPointerDevice(PT_TOUCH) failed: %lu\n", GetLastError());
        touchDeviceFailed_ = true;
        return false;
    }
    return true;
}

void InputInjector::FlushTouches(UINT32 focusId, UINT32 focusPhaseFlag)
{
    if (touches_.empty())
        return;

    std::vector<POINTER_TYPE_INFO> infos;
    infos.reserve(touches_.size());

    for (const auto& entry : touches_) {
        const UINT32 id = entry.first;
        const TouchContact& c = entry.second;
        UINT32 flags;
        if (id == focusId) {
            if (focusPhaseFlag & POINTER_FLAG_UP) {
                // UP or UP|CANCELED: leave interactive state (no INCONTACT).
                flags = POINTER_FLAG_INRANGE | focusPhaseFlag;
            } else if (focusPhaseFlag & POINTER_FLAG_DOWN) {
                flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN;
            } else {
                flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;
            }
        } else {
            // Sibling contacts stay in contact for the same frame.
            flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;
        }

        POINTER_TYPE_INFO info{};
        FillTouchInfo(info, id, c.pt, flags);
        infos.push_back(info);
    }

    if (!InjectSyntheticPointerInput(touchDevice_, infos.data(), static_cast<UINT32>(infos.size())) &&
        !touchInjectFailed_) {
        touchInjectFailed_ = true;
        fprintf(stderr, "InjectSyntheticPointerInput(PT_TOUCH) failed: %lu (n=%zu, focus=%u flags=0x%08X)\n",
                GetLastError(), infos.size(), focusId, focusPhaseFlag);
    }
}

void InputInjector::ReleaseAllTouches()
{
    if (touchDevice_ == nullptr || touches_.empty()) {
        touches_.clear();
        return;
    }

    // Lift every contact with UP in one frame (siblings as UP too).
    std::vector<POINTER_TYPE_INFO> infos;
    infos.reserve(touches_.size());
    for (const auto& entry : touches_) {
        POINTER_TYPE_INFO info{};
        FillTouchInfo(info, entry.first, entry.second.pt, POINTER_FLAG_INRANGE | POINTER_FLAG_UP);
        infos.push_back(info);
    }
    InjectSyntheticPointerInput(touchDevice_, infos.data(), static_cast<UINT32>(infos.size()));
    touches_.clear();
}

void InputInjector::HandleTouch(const TouchMsg& touch)
{
    if (!EnsureTouchDevice())
        return;

    const POINT pt = ScreenPoint(touch.x, touch.y);
    const UINT32 id = touch.id;

    switch (touch.phase) {
        case TouchPhase::Began:
            touches_[id] = TouchContact{pt, true};
            FlushTouches(id, POINTER_FLAG_DOWN);
            break;
        case TouchPhase::Moved:
            if (auto it = touches_.find(id); it == touches_.end() || !it->second.down) {
                // Missed began (or reconnect mid-drag): synthesize a down.
                touches_[id] = TouchContact{pt, true};
                FlushTouches(id, POINTER_FLAG_DOWN);
            } else {
                it->second.pt = pt;
                FlushTouches(id, POINTER_FLAG_UPDATE);
            }
            break;
        case TouchPhase::Ended:
        case TouchPhase::Cancelled: {
            auto it = touches_.find(id);
            if (it == touches_.end())
                break;
            it->second.pt = pt;
            const UINT32 upFlags = (touch.phase == TouchPhase::Cancelled)
                                       ? (POINTER_FLAG_UP | POINTER_FLAG_CANCELED)
                                       : POINTER_FLAG_UP;
            FlushTouches(id, upFlags);
            touches_.erase(it);
            break;
        }
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
    if (touchDevice_ != nullptr)
        DestroySyntheticPointerDevice(touchDevice_);
    if (penDevice_ != nullptr)
        DestroySyntheticPointerDevice(penDevice_);
}

POINT InputInjector::ScreenPoint(double nx, double ny) const
{
    // Pen/touch injection takes physical pixels relative to the *top-left of the
    // virtual screen*, not SendInput's 0..65535 space and not raw desktop
    // coordinates: MonitorRect() is GetMonitorInfo's rcMonitor, so a monitor
    // left of or above the primary is negative there, and Windows drops such
    // frames without an error (InjectSyntheticPointerInput still returns TRUE).
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
    lastPenPoint_ = pt;

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
            // still hold the pen down - release it first, or the button stays
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

void InputInjector::EndSession()
{
    ReleaseAllTouches();

    if (penDevice_ == nullptr)
        return;

    ReleasePenIfDown(lastPenPoint_);
    if (penInRange_) {
        // No INRANGE flag: tells Windows the pen left hover range for good.
        InjectPen(POINTER_FLAG_UPDATE, lastPenPoint_, 0.0, 0.0, kPencilAltitudeUpright);
        penInRange_ = false;
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
