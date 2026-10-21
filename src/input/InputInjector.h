#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <unordered_map>

#include "net/Protocol.h"

namespace od {

// Maps receiver touch/scroll control messages to Win32 input.
// Finger touch uses CreateSyntheticPointerDevice(PT_TOUCH) (Windows 10 1809+)
// with POINTER_TOUCH_INFO - real multi-contact capable touch, not mouse.
// Coordinates are normalized [0,1], origin top-left, relative to the video
// image - mapped to the virtual monitor's rect, then to virtual-screen-relative
// pixels (same origin shift as the pen path).
// Scroll stays on SendInput wheel notches.
// Apple Pencil takes a separate PT_PEN device fed with POINTER_PEN_INFO
// (pressure, tilt, hover). Windows synthesizes mouse messages for
// non-pointer-aware apps from both; WinTab-only apps do not see them.
class InputInjector {
public:
    ~InputInjector();

    InputInjector() = default;
    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    // Rect of the virtual monitor within the Windows virtual desktop
    // (MTT ExistingMonitor rect). Must be set before touches arrive;
    // update again on rotation (new hello -> new rect).
    void SetMonitorRect(const RECT& rect) { monitorRect_ = rect; }

    // Share of the encoded frame the desktop fills (capture / encode size, 1 =
    // whole frame). Below 1 when the encoder pads to whole macroblocks: the
    // iPad's normalized touch covers the padded frame, not just the desktop.
    void SetContentFraction(double fx, double fy)
    {
        contentFx_ = fx;
        contentFy_ = fy;
    }

    void HandleTouch(const TouchMsg& touch);
    void HandleScroll(const ScrollMsg& scroll);
    void HandlePencil(const PencilMsg& pencil);
    void HandleProximity(const ProximityMsg& proximity);

    // Call when a connection ends. Pen/touch devices outlive a reconnect, so a
    // link that drops mid-stroke would otherwise leave injected contacts down.
    void EndSession();

private:
    struct TouchContact {
        POINT pt{};
        bool down = false;
    };

    POINT ScreenPoint(double nx, double ny) const;
    bool EnsurePenDevice();
    bool EnsureTouchDevice();
    void InjectPen(UINT32 flags, POINT pt, double pressure, double azimuth, double altitude);
    void ReleasePenIfDown(POINT pt);
    // Inject every active contact. focusId gets focusPhaseFlag (DOWN / UPDATE /
    // UP [| CANCELED]); every other down contact gets INCONTACT|UPDATE.
    void FlushTouches(UINT32 focusId, UINT32 focusPhaseFlag);
    void ReleaseAllTouches();

    RECT monitorRect_{};
    std::atomic<double> contentFx_{1.0};
    std::atomic<double> contentFy_{1.0};

    HSYNTHETICPOINTERDEVICE touchDevice_ = nullptr;
    bool touchDeviceFailed_ = false;
    bool touchInjectFailed_ = false;
    std::unordered_map<UINT32, TouchContact> touches_;

    HSYNTHETICPOINTERDEVICE penDevice_ = nullptr;
    bool penDeviceFailed_ = false; // creation failed once -> stop retrying per event
    bool injectFailed_ = false;    // log the first injection failure only
    bool penDown_ = false;
    bool penInRange_ = false;
    POINT lastPenPoint_{}; // where to release from if the link dies mid-stroke
};

} // namespace od
