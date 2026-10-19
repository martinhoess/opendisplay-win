#pragma once

#include <cstdint>
#include <string>

namespace od {

enum class PauseReason { None, Locked, Asleep, Admin, Paused };

const char* PauseReasonName(PauseReason reason);

// Draws the pause overlay into a tightly packed BGRA image (stride w*4) in
// place: picture dimmed to ~40%, a centered rounded card with the reason
// (Segoe UI Semibold + color emoji). Software Direct2D on a WIC bitmap, so it
// works for either capture path and with no capture device at all.
bool RenderPauseOverlay(uint8_t* bgra, uint32_t width, uint32_t height, PauseReason reason);

// Test hook (--render-pause-previews DIR): one PNG per reason, drawn over a
// synthetic desktop-like image of width x height.
bool SavePauseOverlayPreviews(const std::wstring& dir, uint32_t width, uint32_t height);

} // namespace od
