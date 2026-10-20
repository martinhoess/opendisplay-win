#pragma once

#include <cstdint>
#include <string>

namespace od {

enum class PauseReason { None, Locked, Asleep, Admin, Paused };

const char* PauseReasonName(PauseReason reason);

// Locked and Admin (secure desktop) never show the desktop: the card goes on a
// plain dark background instead of the dimmed last picture.
bool PauseHidesDesktop(PauseReason reason);

// Fills a tightly packed BGRA image with that plain background (#111 with a
// faint vertical gradient), overwriting whatever was there.
void FillPauseBackground(uint8_t* bgra, uint32_t width, uint32_t height);

// Draws the pause overlay into a tightly packed BGRA image (stride w*4) in
// place: picture dimmed to ~40% (or, for PauseHidesDesktop reasons, replaced
// by FillPauseBackground first), a centered rounded card with the reason
// (Segoe UI Semibold + color emoji). Software Direct2D on a WIC bitmap, so it
// works for either capture path and with no capture device at all.
bool RenderPauseOverlay(uint8_t* bgra, uint32_t width, uint32_t height, PauseReason reason);

// Test hook (--render-pause-previews DIR): one PNG per reason, drawn over a
// synthetic desktop-like image of width x height.
bool SavePauseOverlayPreviews(const std::wstring& dir, uint32_t width, uint32_t height);

} // namespace od
