#include "app/PauseSignals.h"

#include <wtsapi32.h>

#include <cwchar>

#pragma comment(lib, "wtsapi32.lib")

namespace od {

std::atomic<bool> PauseSignals::locked{false};
std::atomic<bool> PauseSignals::displayOff{false};

HANDLE PauseSignals::WakeEvent()
{
    static HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return event;
}

void PauseSignals::Notify()
{
    if (HANDLE e = WakeEvent())
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
