#include "app/PauseSignals.h"

#include <wtsapi32.h>

#include <algorithm>
#include <cwchar>
#include <mutex>
#include <vector>

#pragma comment(lib, "wtsapi32.lib")

namespace od {

std::atomic<bool> PauseSignals::locked{false};
std::atomic<bool> PauseSignals::displayOff{false};
std::atomic<uint32_t> PauseSignals::displayChangeGen{0};

namespace {

// One auto-reset event per waiting thread (one per sender), all set by
// Notify(): a single shared auto-reset event would wake only one of several
// paused senders.
std::mutex g_wakeMutex;
std::vector<HANDLE> g_wakeEvents;

struct ThreadWake {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ThreadWake()
    {
        std::lock_guard<std::mutex> lock(g_wakeMutex);
        if (event)
            g_wakeEvents.push_back(event);
    }
    ~ThreadWake()
    {
        std::lock_guard<std::mutex> lock(g_wakeMutex);
        g_wakeEvents.erase(std::remove(g_wakeEvents.begin(), g_wakeEvents.end(), event), g_wakeEvents.end());
        if (event)
            CloseHandle(event);
    }
};

} // namespace

HANDLE PauseSignals::WakeEvent()
{
    thread_local ThreadWake wake;
    return wake.event;
}

void PauseSignals::Notify()
{
    std::lock_guard<std::mutex> lock(g_wakeMutex);
    for (HANDLE e : g_wakeEvents)
        SetEvent(e);
}

InputDesktop CurrentInputDesktop()
{
    HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desk)
        return InputDesktop::Secure; // access denied: the secure desktop
    wchar_t name[64] = {};
    DWORD needed = 0;
    const BOOL ok = GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name), &needed);
    CloseDesktop(desk);
    if (!ok)
        return InputDesktop::Other;
    if (_wcsicmp(name, L"Default") == 0)
        return InputDesktop::Default;
    if (_wcsicmp(name, L"Winlogon") == 0)
        return InputDesktop::Secure;
    return InputDesktop::Other;
}

bool IsSessionLocked()
{
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, &buffer,
                                     &bytes) ||
        !buffer)
        return PauseSignals::locked.load();
    bool lockedNow = PauseSignals::locked.load();
    if (bytes >= sizeof(WTSINFOEXW)) {
        const auto* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
        if (info->Level == 1)
            lockedNow = info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
    }
    WTSFreeMemory(buffer);
    return lockedNow;
}

} // namespace od
