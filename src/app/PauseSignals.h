#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace od {

// Session/display state the tray window hears about (WTS lock/unlock, console
// display on/off) and the capture loops act on: a locked or dark PC pauses the
// stream instead of dropping it. Set from the tray's window thread only.
struct PauseSignals {
    static std::atomic<bool> locked;     // WTS_SESSION_LOCK seen, no unlock yet
    static std::atomic<bool> displayOff; // GUID_CONSOLE_DISPLAY_STATE == off
    // Bumped on every WM_DISPLAYCHANGE (mode / refresh / topology): senders
    // re-read what they derive from the display mode when it moves.
    static std::atomic<uint32_t> displayChangeGen;
    // Auto-reset event, signalled on every change above, so a paused sender
    // wakes up at once instead of at its next check.
    static HANDLE WakeEvent();
    static void Notify();
};

// What the input desktop is right now. Secure = Winlogon (lock screen, UAC
// consent, Ctrl+Alt+Del) or not openable from this process.
enum class InputDesktop { Default, Secure, Other };
InputDesktop CurrentInputDesktop();

// WTS session flags for this session (authoritative lock state, also covers a
// lock that happened before the tray started listening).
bool IsSessionLocked();

} // namespace od
