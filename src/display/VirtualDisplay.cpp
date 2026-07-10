#include "display/VirtualDisplay.h"

#include "parsec-vdd.h"

#include <chrono>
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
    while (keepAliveRunning_) {
        parsec_vdd::VddUpdate(device_);
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

    deviceName_ = adapter.DeviceName;
    bool attached = (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);

    if (!attached) {
        int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int y = GetSystemMetrics(SM_YVIRTUALSCREEN);

        mode.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
        mode.dmPosition.x = x;
        mode.dmPosition.y = y;
        mode.dmPelsWidth = targetWidth_;
        mode.dmPelsHeight = targetHeight_;
        mode.dmBitsPerPel = 32;
        mode.dmDisplayFrequency = targetHz_;

        ChangeDisplaySettingsExW(deviceName_.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        LONG r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        if (r != DISP_CHANGE_SUCCESSFUL) {
            fprintf(stderr, "ChangeDisplaySettingsEx (attach) failed: %ld\n", r);
            return false;
        }
    } else if (EnumDisplaySettingsW(deviceName_.c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
        if (mode.dmPelsWidth != targetWidth_ || mode.dmPelsHeight != targetHeight_) {
            mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
            mode.dmPelsWidth = targetWidth_;
            mode.dmPelsHeight = targetHeight_;
            mode.dmDisplayFrequency = targetHz_;
            ChangeDisplaySettingsExW(deviceName_.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
            ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        }
    }

    return WaitUntil([&] { return QueryMonitorRect(); }, 3000);
}

bool VirtualDisplay::QueryMonitorRect()
{
    struct Ctx {
        const std::wstring* name;
        RECT rect;
        bool found;
    } ctx{&deviceName_, {}, false};

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

    if (!ctx.found)
        return false;

    monitorRect_ = ctx.rect;
    return true;
}

} // namespace od
