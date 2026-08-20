#include "display/VirtualDisplay.h"

#include "app/Log.h"

#include "parsec-vdd.h"

#include <shellapi.h>

#include <chrono>
#include <climits>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

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

// GDI device names of the parsec virtual monitors currently attached to the
// desktop.
//
// The driver exposes all VDD_MAX_DISPLAYS slots as *permanent* display device
// entries (e.g. \\.\DISPLAY21..36), so the device string alone identifies the
// adapter, never our own monitor: picking the first match would hand a second
// sender the monitor the first one is already capturing and injecting into.
// Only the set of attached names, diffed across our own VddAddDisplay, tells
// them apart.
std::set<std::wstring> AttachedParsecDisplays()
{
    std::set<std::wstring> out;
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dev{};
        dev.cb = sizeof(dev);
        if (!EnumDisplayDevicesW(nullptr, i, &dev, 0))
            break;
        if ((dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0 &&
            wcsstr(dev.DeviceString, L"Parsec Virtual Display Adapter") != nullptr)
            out.insert(dev.DeviceName);
    }
    return out;
}

// Serializes "snapshot, add, claim the new name" machine-wide. Two senders
// adding a display at the same moment would otherwise both see the same new
// name in their diff and drive the same monitor.
//
// Held() says whether we really own it. Going ahead without the lock is exactly
// the race the lock exists for, so the caller aborts the add instead and lets
// the reconnect try again — the sender is in a retry loop anyway, while a
// hijacked monitor is silent and permanent.
class VddClaimLock {
public:
    VddClaimLock()
    {
        handle_ = CreateMutexW(nullptr, FALSE, L"Global\\opendisplay-win-vdd-claim");
        if (handle_ == nullptr)
            return;
        // WAIT_ABANDONED counts as owned: the previous holder died mid-add, and
        // the wait handed us the mutex.
        DWORD result = WaitForSingleObject(handle_, 15000);
        held_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ~VddClaimLock()
    {
        if (handle_ == nullptr)
            return;
        if (held_)
            ReleaseMutex(handle_); // never on a mutex we don't own
        CloseHandle(handle_);
    }

    bool Held() const { return held_; }

    VddClaimLock(const VddClaimLock&) = delete;
    VddClaimLock& operator=(const VddClaimLock&) = delete;

private:
    HANDLE handle_ = nullptr;
    bool held_ = false;
};

// Removes leftover *non-present* parsec virtual-monitor devices and returns how
// many were removed. Each VddAddDisplay mints a monitor with a fresh UID, and
// when a previous run's process died the monitor was unplugged but its devnode
// lingers as a phantom in Device Manager. We only touch monitors whose
// instance id carries the parsec display id AND that are not currently present
// — so the live monitor (present) and every physical monitor are never
// affected. Best-effort. Deliberately NOT run automatically (device removal is
// too invasive for the runtime path); exposed as an explicit one-off instead.
int RemoveGhostMonitors()
{
    int removed = 0;
    // Monitor device class {4d36e96e-e325-11ce-bfc1-08002be10318}.
    static const GUID kMonitorClass = {
        0x4d36e96e, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

    // No DIGCF_PRESENT: include phantom (non-present) devices too.
    HDEVINFO devInfo = SetupDiGetClassDevsW(&kMonitorClass, nullptr, nullptr, 0);
    if (devInfo == INVALID_HANDLE_VALUE)
        return removed;

    SP_DEVINFO_DATA did{};
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &did); ++i) {
        wchar_t instanceId[256];
        if (!SetupDiGetDeviceInstanceIdW(devInfo, &did, instanceId, 256, nullptr))
            continue;

        // Only the parsec virtual monitor (VDD_DISPLAY_ID = "PSCCDD0").
        if (wcsstr(instanceId, L"PSCCDD0") == nullptr)
            continue;

        // Present devnode => it's the live monitor; never remove it.
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, did.DevInst, 0) == CR_SUCCESS)
            continue;

        if (SetupDiRemoveDevice(devInfo, &did)) // phantom -> drop it (best-effort)
            ++removed;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return removed;
}

// --- Custom-resolution registry (HKLM\SOFTWARE\Parsec\vdd\0..4) ---------------
// parsec-vdd's default EDID doesn't list an iPad's native resolution, so we add
// it as a custom mode. Up to 5 slots — enough for a couple of iPads (both
// orientations each). Writing needs admin; reading doesn't.

struct Res {
    DWORD w = 0, h = 0, hz = 0;
};

std::vector<Res> ReadRegisteredResolutions()
{
    std::vector<Res> out;
    for (int i = 0; i < 5; ++i) {
        wchar_t sub[64];
        swprintf_s(sub, L"SOFTWARE\\Parsec\\vdd\\%d", i);
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            continue;
        auto getDword = [&](const wchar_t* name, DWORD& v) {
            DWORD size = sizeof(v), type = 0;
            return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS &&
                   type == REG_DWORD;
        };
        Res r;
        bool ok = getDword(L"width", r.w) && getDword(L"height", r.h);
        if (!getDword(L"hz", r.hz))
            r.hz = 60;
        RegCloseKey(key);
        if (ok && r.w && r.h)
            out.push_back(r);
    }
    return out;
}

bool IsResolutionRegistered(uint32_t w, uint32_t h)
{
    for (const auto& r : ReadRegisteredResolutions())
        if (r.w == w && r.h == h)
            return true;
    return false;
}

// Writes the list into slots 0.., deleting any leftover slots. Needs admin;
// returns false (without side effects that matter) if not elevated.
bool WriteResolutionSlots(const std::vector<Res>& list)
{
    for (int i = 0; i < 5; ++i) {
        wchar_t sub[64];
        swprintf_s(sub, L"SOFTWARE\\Parsec\\vdd\\%d", i);
        if (i < static_cast<int>(list.size())) {
            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                &key, nullptr) != ERROR_SUCCESS)
                return false; // non-admin: HKLM write denied
            DWORD w = list[i].w, h = list[i].h, z = list[i].hz;
            RegSetValueExW(key, L"width", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&w), sizeof(w));
            RegSetValueExW(key, L"height", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&h), sizeof(h));
            RegSetValueExW(key, L"hz", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&z), sizeof(z));
            RegCloseKey(key);
        } else {
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, sub); // best-effort tidy of unused slots
        }
    }
    return true;
}

} // namespace

VirtualDisplay::VirtualDisplay() = default;

VirtualDisplay::~VirtualDisplay()
{
    Close();
}

int VirtualDisplay::CleanupGhostMonitors()
{
    return RemoveGhostMonitors();
}

bool VirtualDisplay::RemoveDisplayIndex(int index)
{
    HANDLE device = parsec_vdd::OpenDeviceHandle(&parsec_vdd::VDD_ADAPTER_GUID);
    if (device == nullptr || device == INVALID_HANDLE_VALUE)
        return false;
    parsec_vdd::VddRemoveDisplay(device, index);
    parsec_vdd::CloseDeviceHandle(device);
    return true;
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
        {
            std::lock_guard<std::mutex> lock(vddMutex_);
            parsec_vdd::VddUpdate(device_);
        }

        // ~once a second, notice if the user dragged the monitor and persist it.
        if (++tick >= 20) {
            tick = 0;
            PollPosition(lastKnownPos);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool VirtualDisplay::RegisterResolutions(uint32_t width, uint32_t height)
{
    // Ensure both this resolution and its rotation (swapped W/H) are among the
    // registered custom modes, preserving what's already there (other iPads).
    std::vector<Res> list = ReadRegisteredResolutions();
    auto ensure = [&](DWORD w, DWORD h) {
        for (const auto& r : list)
            if (r.w == w && r.h == h)
                return;
        list.push_back({w, h, 60});
    };
    ensure(width, height);
    ensure(height, width);

    // Only 5 slots — if we overflow, keep the most recently needed ones.
    if (list.size() > 5)
        list.erase(list.begin(), list.end() - 5);

    return WriteResolutionSlots(list); // needs admin
}

bool VirtualDisplay::SelfElevateRegister(uint32_t width, uint32_t height)
{
    // Relaunch ourselves elevated to do just the one HKLM write (single UAC
    // prompt), then continue running un-elevated. Registering a new iPad's
    // resolution is the only admin-needing step.
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
        return false;
    std::wstring args = L"--register-resolution " + std::to_wstring(width) + L" " + std::to_wstring(height);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr)
        return false; // user declined the UAC prompt, or launch failed

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0 && IsResolutionRegistered(width, height);
}

bool VirtualDisplay::EnsureResolution(uint32_t width, uint32_t height, uint32_t hz)
{
    if (!IsOpen())
        return false;

    // Reconnects (same panel, no rotation) reuse the monitor — but only if it's
    // actually still there. The driver drops the virtual display across a
    // standby/resume (and other resets), leaving displayIndex_ pointing at a
    // monitor that no longer exists; reusing it blindly would fail forever and
    // the app would just loop "connecting". If QueryMonitorRect() can't find it,
    // fall through to the remove + re-add path below (which cleans up the stale
    // index, with the settle delay, and re-attaches).
    if (displayIndex_ >= 0 && targetWidth_ == width && targetHeight_ == height && targetHz_ == hz &&
        QueryMonitorRect())
        return true;

    targetWidth_ = width;
    targetHeight_ = height;
    targetHz_ = hz;

    // The resolution must be a registered custom mode. If it isn't, register it
    // (needs admin): do it directly when already elevated, otherwise self-
    // elevate a one-off. A known iPad is already registered, so this is a
    // one-time UAC prompt the first time a new panel size is seen.
    if (!IsResolutionRegistered(width, height)) {
        if (!RegisterResolutions(width, height) && !SelfElevateRegister(width, height)) {
            Logf(identity_, "resolution %ux%u not registered (needs admin once; UAC declined?)\n", width, height);
            return false;
        }
    }

    if (displayIndex_ >= 0) {
        std::wstring previous;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            previous = deviceName_;
        }
        {
            std::lock_guard<std::mutex> lock(vddMutex_);
            parsec_vdd::VddRemoveDisplay(device_, displayIndex_);
        }
        displayIndex_ = -1;
        // Wait for the monitor to actually leave the desktop before the
        // snapshot below is taken. The driver may hand the very same slot back
        // on the next add, and a name still listed as attached would land in
        // the "before" set — the diff would then never find our new monitor.
        if (!previous.empty())
            WaitUntil([&] { return AttachedParsecDisplays().count(previous) == 0; }, 2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Held across add + claim so a concurrently starting sender can't take the
    // monitor this add is about to produce.
    VddClaimLock claimLock;
    if (!claimLock.Held()) {
        Logf(identity_, "another sender held the display claim for 15s, retrying on the next connect\n");
        return false;
    }
    std::set<std::wstring> attachedBefore = AttachedParsecDisplays();

    int idx;
    {
        std::lock_guard<std::mutex> lock(vddMutex_);
        idx = parsec_vdd::VddAddDisplay(device_);
    }
    if (idx < 0) {
        Logf(identity_, "VddAddDisplay failed\n");
        return false;
    }
    displayIndex_ = idx;

    // If the monitor didn't attach (e.g. the desktop wasn't settled yet at
    // logon), drop the display so the *next* reconnect re-adds and re-attaches
    // from scratch. Without this, the early-return above would keep querying a
    // never-attached display forever — the app would loop "connecting" and
    // never recover once conditions became good.
    if (!FindMonitorGeometry(attachedBefore)) {
        {
            std::lock_guard<std::mutex> lock(vddMutex_);
            parsec_vdd::VddRemoveDisplay(device_, displayIndex_);
        }
        displayIndex_ = -1;
        return false;
    }
    return true;
}

bool VirtualDisplay::FindMonitorGeometry(const std::set<std::wstring>& attachedBefore)
{
    // Our monitor is the parsec display that attached itself since the
    // snapshot — the driver attaches an added display on its own, before we set
    // any mode on it. Polled because enumeration lags the add by a moment.
    std::wstring name;
    bool foundMonitor = WaitUntil(
        [&] {
            for (const std::wstring& attached : AttachedParsecDisplays()) {
                if (attachedBefore.count(attached) == 0) {
                    name = attached;
                    return true;
                }
            }
            return false;
        },
        3000);

    if (!foundMonitor) {
        Logf(identity_, "no new virtual monitor attached after add\n");
        return false;
    }

    // Which monitor this sender claimed — the one line that shows two senders
    // ended up on two different displays.
    Logf(identity_, "claimed virtual monitor %ls\n", name.c_str());

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

    LONG rDev = ChangeDisplaySettingsExW(name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    LONG r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        Logf(identity_, "ChangeDisplaySettingsEx failed: apply=%ld dev=%ld\n", r, rDev);
        return false;
    }

    // The device call has its own result, and it can fail (-2 BADMODE) while
    // the global apply still reports success — the monitor is then left on
    // whatever mode Windows had persisted for this display path, and the
    // picture reaches the iPad letterboxed. Worth its own line: this is the
    // difference between "we set the panel size" and "we got lucky".
    DEVMODEW actual{};
    actual.dmSize = sizeof(actual);
    if (EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &actual) &&
        (actual.dmPelsWidth != targetWidth_ || actual.dmPelsHeight != targetHeight_))
        Logf(identity_, "monitor %ls stayed at %lux%lu, wanted %ux%u (dev-set=%ld)\n", name.c_str(), actual.dmPelsWidth,
             actual.dmPelsHeight, targetWidth_, targetHeight_, rDev);

    bool got = WaitUntil([&] { return QueryMonitorRect(); }, 3000);

    // A stale saved position can be off-screen after a monitor-layout change and
    // refuse to attach. Fall back to the default right edge (and persist it) so
    // we recover instead of looping add/remove on every reconnect.
    if (!got) {
        int defX = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int defY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        if (defX != posX || defY != posY) {
            mode.dmPosition.x = defX;
            mode.dmPosition.y = defY;
            ChangeDisplaySettingsExW(name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
            ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
            got = WaitUntil([&] { return QueryMonitorRect(); }, 3000);
            if (got)
                SavePosition(defX, defY);
        }
    }

    if (!got)
        Logf(identity_, "monitor %ls did not attach at %d,%d %ux%u (dev-set=%ld)\n", name.c_str(), posX, posY,
             targetWidth_, targetHeight_, rDev);
    return got;
}

RECT VirtualDisplay::MonitorRect() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return monitorRect_;
}

bool VirtualDisplay::QueryMonitorRect()
{
    RECT r{};
    if (!GetMonitorRectByName(deviceName_, r))
        return false;
    std::lock_guard<std::mutex> lock(stateMutex_);
    monitorRect_ = r;
    return true;
}

void VirtualDisplay::SetIdentity(const std::string& id)
{
    identity_ = id;
}

std::wstring VirtualDisplay::PositionValueName(const wchar_t* base) const
{
    // One saved position per iPad: several senders each drive their own
    // monitor, and a shared position would stack them on the same spot.
    if (identity_.empty())
        return std::wstring(base);

    // The identity ends up in a registry value name, so keep it to characters
    // that read back cleanly.
    std::wstring name = std::wstring(base) + L"_";
    for (char c : identity_) {
        bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        name += plain ? static_cast<wchar_t>(c) : L'_';
    }
    return name;
}

bool VirtualDisplay::LoadSavedPosition(int& x, int& y) const
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\opendisplay-win", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS)
        return false;

    auto getDword = [&](const std::wstring& name, int& out) {
        DWORD value = 0, size = sizeof(value), type = 0;
        bool ok = RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) ==
                      ERROR_SUCCESS &&
                  type == REG_DWORD;
        if (ok)
            out = static_cast<int>(value); // round-trips negative coords via the bit pattern
        return ok;
    };

    bool ok = getDword(PositionValueName(L"monitorX"), x) && getDword(PositionValueName(L"monitorY"), y);

    // One-time migration from the single-iPad layout: the first identity that
    // asks inherits the old shared position, then it is dropped so the next
    // iPad starts at the default spot instead of on top of this one.
    if (!ok && !identity_.empty() && getDword(L"monitorX", x) && getDword(L"monitorY", y)) {
        ok = true;
        RegDeleteValueW(key, L"monitorX");
        RegDeleteValueW(key, L"monitorY");
    }

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
    RegSetValueExW(key, PositionValueName(L"monitorX").c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&vx),
                   sizeof(vx));
    RegSetValueExW(key, PositionValueName(L"monitorY").c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&vy),
                   sizeof(vy));
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
        // next launch restores this position, and update the live rect so
        // input mapping follows the move without waiting for a reconnect.
        SavePosition(r.left, r.top);
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            monitorRect_ = r;
        }
        lastKnown = {r.left, r.top};
    }
}

} // namespace od
