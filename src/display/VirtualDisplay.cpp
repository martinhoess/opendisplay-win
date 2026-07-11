#include "display/VirtualDisplay.h"

#include "parsec-vdd.h"

#include <chrono>
#include <climits>
#include <cstdio>

namespace od {

namespace {

// Windows only re-reads a virtual display's EDID/mode list on (re)connect,
// so after a driver-level add/remove we have to wait for the OS to notice
// before EnumDisplayDevices/EnumDisplayMonitors will see it. Polled rather
// than a single fixed sleep since enumeration timing varies by system load.
template <typename Predicate>
bool WaitUntil(Predicate pred, int timeoutMs, int pollMs = 150)
{
    int waited = 0;
    while (waited < timeoutMs) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        waited += pollMs;
    }
    return pred();
}

// Looks up the desktop rect of the monitor with the given GDI device name
// (e.g. L"\\.\DISPLAY3"). Returns false if no such monitor is currently
// attached. Shared by QueryMonitorRect and the position poll.
bool GetMonitorRectByName(const std::wstring& name, RECT& out)
{
    struct Ctx {
        const std::wstring* name;
        RECT rect;
        bool found;
    } ctx{&name, {}, false};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* ctx = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(hMon, &info) && *ctx->name == info.szDevice) {
                ctx->rect = info.rcMonitor;
                ctx->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found)
        out = ctx.rect;
    return ctx.found;
}

} // namespace

VirtualDisplay::VirtualDisplay() = default;

VirtualDisplay::~VirtualDisplay()
{
    Close();
}

bool VirtualDisplay::Open()
{
    device_ = parsec_vdd::OpenDeviceHandle(&parsec_vdd::VDD_ADAPTER_GUID);
    if (device_ == nullptr || device_ == INVALID_HANDLE_VALUE) {
        device_ = nullptr;
        return false;
    }

    keepAliveRunning_ = true;
    keepAliveThread_ = std::thread([this] { KeepAliveLoop(); });
    return true;
}

void VirtualDisplay::Close()
{
    keepAliveRunning_ = false;
    if (keepAliveThread_.joinable())
        keepAliveThread_.join();

    if (displayIndex_ >= 0 && device_) {
        parsec_vdd::VddRemoveDisplay(device_, displayIndex_);
        displayIndex_ = -1;
    }
    if (device_) {
        parsec_vdd::CloseDeviceHandle(device_);
        device_ = nullptr;
    }
}

void VirtualDisplay::KeepAliveLoop()
{
    POINT lastKnownPos{INT_MIN, INT_MIN}; // INT_MIN = "not observed yet"
    int tick = 0;

    while (keepAliveRunning_) {
        parsec_vdd::VddUpdate(device_);

        // ~once a second, notice if the user dragged the monitor and persist it.
        if (++tick >= 20) {
            tick = 0;
            PollPosition(lastKnownPos);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool VirtualDisplay::WriteCustomResolutionRegistry(uint32_t width, uint32_t height, uint32_t hz)
{
    HKEY key = nullptr;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Parsec\\vdd\\0", 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (r != ERROR_SUCCESS)
        return false;

    auto setDword = [&](const wchar_t* name, DWORD value) {
        return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) ==
               ERROR_SUCCESS;
    };

    bool ok = setDword(L"width", width) && setDword(L"height", height) && setDword(L"hz", hz);
    RegCloseKey(key);
    return ok;
}

bool VirtualDisplay::EnsureResolution(uint32_t width, uint32_t height, uint32_t hz)
{
    if (!IsOpen())
        return false;

    // Reconnects (same panel, no rotation) shouldn't flicker-rebuild the
    // monitor — only a genuine dimension change needs a remove/re-add.
    if (displayIndex_ >= 0 && targetWidth_ == width && targetHeight_ == height && targetHz_ == hz)
        return QueryMonitorRect();

    targetWidth_ = width;
    targetHeight_ = height;
    targetHz_ = hz;

    if (!WriteCustomResolutionRegistry(width, height, hz)) {
        fprintf(stderr, "VirtualDisplay: failed to write HKLM custom resolution (need admin?)\n");
        return false;
    }

    if (displayIndex_ >= 0) {
        parsec_vdd::VddRemoveDisplay(device_, displayIndex_);
        displayIndex_ = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    int idx = parsec_vdd::VddAddDisplay(device_);
    if (idx < 0) {
        fprintf(stderr, "VddAddDisplay failed\n");
        return false;
    }
    displayIndex_ = idx;

    return FindMonitorGeometry();
}

bool VirtualDisplay::FindMonitorGeometry()
{
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);

    bool foundAdapter = WaitUntil(
        [&] {
            for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i) {
                if (wcsstr(adapter.DeviceString, L"Parsec Virtual Display Adapter") != nullptr)
                    return true;
            }
            return false;
        },
        3000);

    if (!foundAdapter) {
        fprintf(stderr, "VirtualDisplay: adapter not found after add\n");
        return false;
    }

    std::wstring name = adapter.DeviceName;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        deviceName_ = name;
    }

    // Restore the position the user last left the monitor at (persisted by the
    // keepalive poll). First run has none, so fall back to the right edge of
    // the virtual desktop and save that as the initial position.
    int posX = 0, posY = 0;
    if (!LoadSavedPosition(posX, posY)) {
        posX = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        posY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        SavePosition(posX, posY);
    }

    // Apply position + resolution in one shot, every add. The monitor's
    // identity changes on each add so Windows never has the position right on
    // its own — we always place it. If it's already correct this is a no-op.
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    mode.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    mode.dmPosition.x = posX;
    mode.dmPosition.y = posY;
    mode.dmPelsWidth = targetWidth_;
    mode.dmPelsHeight = targetHeight_;
    mode.dmBitsPerPel = 32;
    mode.dmDisplayFrequency = targetHz_;

    ChangeDisplaySettingsExW(name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    LONG r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        fprintf(stderr, "ChangeDisplaySettingsEx failed: %ld\n", r);
        return false;
    }

    return WaitUntil([&] { return QueryMonitorRect(); }, 3000);
}

bool VirtualDisplay::QueryMonitorRect()
{
    RECT r{};
    if (!GetMonitorRectByName(deviceName_, r))
        return false;
    monitorRect_ = r;
    return true;
}

bool VirtualDisplay::LoadSavedPosition(int& x, int& y) const
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\opendisplay-win", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    auto getDword = [&](const wchar_t* name, int& out) {
        DWORD value = 0, size = sizeof(value), type = 0;
        bool ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
                  type == REG_DWORD;
        if (ok)
            out = static_cast<int>(value); // round-trips negative coords via the bit pattern
        return ok;
    };

    bool ok = getDword(L"monitorX", x) && getDword(L"monitorY", y);
    RegCloseKey(key);
    return ok;
}

void VirtualDisplay::SavePosition(int x, int y) const
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\opendisplay-win", 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    DWORD vx = static_cast<DWORD>(x), vy = static_cast<DWORD>(y);
    RegSetValueExW(key, L"monitorX", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&vx), sizeof(vx));
    RegSetValueExW(key, L"monitorY", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&vy), sizeof(vy));
    RegCloseKey(key);
}

void VirtualDisplay::PollPosition(POINT& lastKnown)
{
    std::wstring name;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        name = deviceName_;
    }
    if (name.empty())
        return; // no monitor yet

    RECT r{};
    if (!GetMonitorRectByName(name, r))
        return; // monitor not currently attached (e.g. mid-reconfigure)

    if (lastKnown.x == INT_MIN) {
        lastKnown = {r.left, r.top}; // first observation is the baseline, don't re-save it
        return;
    }
    if (r.left != lastKnown.x || r.top != lastKnown.y) {
        // The user dragged the monitor in Display Settings — remember it so the
        // next launch restores this position instead of the default.
        SavePosition(r.left, r.top);
        lastKnown = {r.left, r.top};
    }
}

} // namespace od
