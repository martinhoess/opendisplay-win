#include "display/ExistingMonitor.h"

#include <atomic>
#include <cwchar>

namespace od {

namespace {

bool DeviceLooksLikeMtt(const DISPLAY_DEVICEW& dev)
{
    // Hardware ID looks like DISPLAY\MTT1337\... ; string is "Generic Monitor (VDD by MTT)"
    if (wcsstr(dev.DeviceID, L"MTT1337") != nullptr)
        return true;
    if (wcsstr(dev.DeviceString, L"VDD by MTT") != nullptr)
        return true;
    return false;
}

std::atomic<uint64_t> g_selfChangeTick{0};
std::atomic<uint64_t> g_selfChangeGen{0};

} // namespace

void NoteSelfDisplayChange()
{
    g_selfChangeTick.store(GetTickCount64());
    g_selfChangeGen.fetch_add(1);
}

bool RecentSelfDisplayChange(uint32_t withinMs)
{
    const uint64_t t = g_selfChangeTick.load();
    return t != 0 && GetTickCount64() - t < withinMs;
}

uint64_t SelfDisplayChangeGeneration()
{
    return g_selfChangeGen.load();
}

bool GetMonitorRectByDeviceName(const std::wstring& deviceName, RECT& out)
{
    struct Ctx {
        const std::wstring* name;
        RECT rect{};
        bool found = false;
    } ctx{&deviceName};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(mon, &mi))
                return TRUE;
            if (c->name->compare(mi.szDevice) == 0) {
                c->rect = mi.rcMonitor;
                c->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (!ctx.found)
        return false;
    out = ctx.rect;
    return true;
}

bool FindMttVirtualMonitorDevice(ExistingMonitor& out, bool requireAttached)
{
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, i, &adapter, 0))
            break;
        const bool adapterAttached = (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        if (requireAttached && !adapterAttached)
            continue;

        for (DWORD m = 0;; ++m) {
            DISPLAY_DEVICEW mon{};
            mon.cb = sizeof(mon);
            if (!EnumDisplayDevicesW(adapter.DeviceName, m, &mon, 0))
                break;
            const bool monAttached = (mon.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
            if (requireAttached && !monAttached)
                continue;
            if (!DeviceLooksLikeMtt(mon) && !DeviceLooksLikeMtt(adapter))
                continue;

            // DXGI / Desktop Duplication want the adapter GDI name (\\.\DISPLAYn),
            // not EnumDisplayDevices' \\.\DISPLAYn\Monitor0 form.
            ExistingMonitor found;
            found.deviceName = adapter.DeviceName;
            found.deviceString = mon.DeviceString[0] ? mon.DeviceString : adapter.DeviceString;
            found.deviceId = mon.DeviceID[0] ? mon.DeviceID : adapter.DeviceID;
            if (monAttached)
                (void)GetMonitorRectByDeviceName(found.deviceName, found.rect);
            out = std::move(found);
            return true;
        }
    }
    return false;
}

bool FindMttVirtualMonitor(ExistingMonitor& out)
{
    return FindMttVirtualMonitorDevice(out, /*requireAttached=*/true);
}

bool AttachMonitorToDesktop(const std::wstring& deviceName, uint32_t width, uint32_t height, uint32_t hz,
                            bool hasPosition, int posX, int posY)
{
    if (deviceName.empty() || width == 0 || height == 0)
        return false;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    // Prefer current/registry mode as a template (works for detached heads).
    if (!EnumDisplaySettingsExW(deviceName.c_str(), ENUM_REGISTRY_SETTINGS, &dm, 0) &&
        !EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        dm = {};
        dm.dmSize = sizeof(dm);
        dm.dmBitsPerPel = 32;
    }

    // First-time default: to the right of the virtual desktop so it does not
    // cover existing displays. Callers with a saved arrangement pass hasPosition.
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    dm.dmPelsWidth = width;
    dm.dmPelsHeight = height;
    dm.dmDisplayOrientation = DMDO_DEFAULT;
    dm.dmPosition.x = hasPosition ? posX : (vx + vw);
    dm.dmPosition.y = hasPosition ? posY : 0;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION | DM_DISPLAYORIENTATION;
    if (hz != 0) {
        dm.dmDisplayFrequency = static_cast<DWORD>(hz);
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    if (dm.dmBitsPerPel == 0) {
        dm.dmBitsPerPel = 32;
        dm.dmFields |= DM_BITSPERPEL;
    }

    NoteSelfDisplayChange();
    const LONG staged =
        ChangeDisplaySettingsExW(deviceName.c_str(), &dm, nullptr,
                                 CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (staged != DISP_CHANGE_SUCCESSFUL && staged != DISP_CHANGE_RESTART)
        return false;
    const LONG applied = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    NoteSelfDisplayChange();
    return applied == DISP_CHANGE_SUCCESSFUL || applied == DISP_CHANGE_RESTART;
}

bool SetMonitorDesktopPosition(const std::wstring& deviceName, int posX, int posY)
{
    if (deviceName.empty())
        return false;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm))
        return false;
    if (dm.dmPosition.x == posX && dm.dmPosition.y == posY)
        return true;
    dm.dmPosition.x = posX;
    dm.dmPosition.y = posY;
    dm.dmFields = DM_POSITION;
    NoteSelfDisplayChange();
    const LONG staged =
        ChangeDisplaySettingsExW(deviceName.c_str(), &dm, nullptr,
                                 CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (staged != DISP_CHANGE_SUCCESSFUL && staged != DISP_CHANGE_RESTART)
        return false;
    const LONG applied = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    NoteSelfDisplayChange();
    return applied == DISP_CHANGE_SUCCESSFUL || applied == DISP_CHANGE_RESTART;
}


bool EnsureMonitorRefresh(const std::wstring& deviceName, uint32_t fps)
{
    if (fps == 0)
        return false;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm))
        return false;
    if (dm.dmDisplayFrequency == static_cast<DWORD>(fps))
        return true;
    dm.dmDisplayFrequency = static_cast<DWORD>(fps);
    dm.dmFields = DM_DISPLAYFREQUENCY;
    NoteSelfDisplayChange();
    const LONG r = ChangeDisplaySettingsExW(deviceName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    NoteSelfDisplayChange();
    return r == DISP_CHANGE_SUCCESSFUL;
}

bool EnsureMonitorMode(const std::wstring& deviceName, uint32_t width, uint32_t height, uint32_t fps)
{
    if (width == 0 || height == 0)
        return false;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm))
        return false;

    const bool orientOk = dm.dmDisplayOrientation == DMDO_DEFAULT;
    const bool sizeOk = dm.dmPelsWidth == width && dm.dmPelsHeight == height;
    const bool fpsOk = fps == 0 || dm.dmDisplayFrequency == static_cast<DWORD>(fps);
    if (orientOk && sizeOk && fpsOk)
        return true;

    dm.dmPelsWidth = width;
    dm.dmPelsHeight = height;
    dm.dmDisplayOrientation = DMDO_DEFAULT;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYORIENTATION;
    if (fps != 0) {
        dm.dmDisplayFrequency = static_cast<DWORD>(fps);
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    NoteSelfDisplayChange();
    const LONG r = ChangeDisplaySettingsExW(deviceName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    NoteSelfDisplayChange();
    return r == DISP_CHANGE_SUCCESSFUL;
}

bool EnsureMonitorLandscapeOrientation(const std::wstring& deviceName)
{
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm))
        return false;
    if (dm.dmDisplayOrientation == DMDO_DEFAULT)
        return true;
    dm.dmDisplayOrientation = DMDO_DEFAULT;
    dm.dmFields = DM_DISPLAYORIENTATION;
    NoteSelfDisplayChange();
    const LONG r = ChangeDisplaySettingsExW(deviceName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    NoteSelfDisplayChange();
    return r == DISP_CHANGE_SUCCESSFUL;
}


bool DetachMonitorFromDesktop(const std::wstring& deviceName)
{
    if (deviceName.empty())
        return false;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    // Zeroed geometry + CDS_UPDATEREGISTRY|CDS_NORESET, then a global apply,
    // is the documented way to detach a display from the desktop.
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
    dm.dmPelsWidth = 0;
    dm.dmPelsHeight = 0;
    dm.dmPosition.x = 0;
    dm.dmPosition.y = 0;
    NoteSelfDisplayChange();
    const LONG staged =
        ChangeDisplaySettingsExW(deviceName.c_str(), &dm, nullptr,
                                 CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (staged != DISP_CHANGE_SUCCESSFUL && staged != DISP_CHANGE_RESTART)
        return false;
    const LONG applied = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    NoteSelfDisplayChange();
    return applied == DISP_CHANGE_SUCCESSFUL || applied == DISP_CHANGE_RESTART;
}
} // namespace od
