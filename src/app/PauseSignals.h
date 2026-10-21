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
    // The calling thread's auto-reset wake event (created on first use);
    // Notify() sets every thread's, so each paused sender wakes at once.
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

// WTSSessionInfoEx SessionFlags only: Locked, Unlocked, or Unknown (query failed).
enum class SessionLock { Locked, Unlocked, Unknown };
SessionLock QuerySessionLock();

// consent.exe (the UAC prompt) is running in this session.
bool IsConsentRunning();

} // namespace od
