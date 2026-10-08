#include "app/TrayApp.h"

#include "app/Config.h"
#include "app/Log.h"
#include "app/SenderApp.h"
#include "app/resources.h"
#include "display/MttVddSettings.h"
#include "net/Mdns.h"

#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

namespace od {

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
// Sender thread -> tray: show a balloon. lParam = heap std::wstring (deleted here).
constexpr UINT WM_APP_NOTIFY = WM_APP + 2;
constexpr UINT kStatusTimerId = 1;
constexpr UINT kStatusTimerMs = 1000;
// WM_DISPLAYCHANGE debounce: a Display Settings Apply fires several changes.
constexpr UINT kDisplayChangeTimerId = 2;
constexpr UINT kDisplayChangeDebounceMs = 1500;
// MTT head already on the desktop at tray start (previous run / reboot): keep
// it this long for the iPad to reconnect, then remove it.
constexpr int kStaleMttHeadGraceMs = 30000;

enum : UINT {
    IDM_CONNECT = 40001,
    IDM_DISCONNECT,
    IDM_SETTINGS,
    IDM_EXIT,

    // One command per configured iPad, IDM_DEVICE_FIRST + index. Toggles that
    // device's sender on its own, so the others keep streaming.
    IDM_DEVICE_FIRST = 41000,
};

const wchar_t* const kWndClass = L"opendisplay-win-tray";

// Dialog layout (dialog units) matching resources.rc - fixed size with tabs.
constexpr int kSettingsWidth = 272;
constexpr int kSettingsHeight = 294;

struct TrayContext {
    HINSTANCE hInstance = nullptr;
    // One sender per configured iPad, index-aligned with cfg.devices: each
    // drives its own virtual monitor, capture, encoder and input injection.
    // unique_ptr because SenderApp owns a thread and can't be moved.
    std::vector<std::unique_ptr<SenderApp>> apps;
    Config cfg;
    NOTIFYICONDATAW nid{};
    HICON iconGreen = nullptr; // at least one iPad streaming
    HICON iconRed = nullptr;   // configured but nothing streaming
    HICON iconGrey = nullptr;  // no iPad configured

    // Names/addresses the iPads advertise over Bonjour. Filled by a background
    // browse so the menu and Settings never wait on the network.
    mutable std::mutex discoveredMutex;
    std::map<std::string, std::string> discovered;       // address -> label
    std::map<std::string, std::string> discoveredById;   // TXT id -> address
    std::map<std::string, std::string> discoveredIdLabel; // TXT id -> label
    std::thread discovery;
    std::atomic<bool> discoveryRunning{false};
};

// One Nearby row = one Wi-Fi (mDNS) iPad candidate.
struct NearbyEntry {
    std::string address;   // Wi-Fi IPv4 from mDNS Nearby
    std::string label;
    std::string id;        // TXT / hello id when known
    std::string usbSerial; // Apple serial when USB is part of this row
    bool viaUsb = false;
    bool viaWifi = false;
};

struct SettingsDlgState {
    TrayContext* tray = nullptr;
    Config* cfg = nullptr;
    std::map<std::string, std::string> pendingIds; // address -> id from Nearby Add
    std::vector<NearbyEntry> nearby;
    std::vector<std::string> devices; // working copy; listbox + advanced edit stay in sync
    int settingsTab = 0; // 0 = General, 1 = Advanced
    bool restartAdminRequested = false;
};

void RebuildSenders(TrayContext* ctx)
{
    for (auto& app : ctx->apps)
        app->Stop();
    ctx->apps.clear();
    for (size_t i = 0; i < ctx->cfg.devices.size(); ++i)
        ctx->apps.push_back(std::make_unique<SenderApp>());
}

// The label an iPad publishes for itself. Its Bonjour instance name is what the
// app's settings call the name - but that field falls back to the system name,
// and iOS hands out a plain "iPad" there, so a generic instance name is passed
// over for the host name (the iOS device name, e.g. "iPad-Pro.local").
std::string ReceiverLabel(const MdnsReceiver& receiver)
{
    bool generic = receiver.instance.empty() || receiver.instance == "iPad" || receiver.instance == "iPhone" ||
                   receiver.instance == "OpenDisplay";
    if (!generic)
        return receiver.instance;

    std::string host = receiver.host;
    const std::string suffix = ".local";
    if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
        host.resize(host.size() - suffix.size());
    return host.empty() ? receiver.instance : host;
}

void RunDiscovery(TrayContext* ctx)
{
    constexpr int kBrowseMs = 800;
    constexpr int kPauseMs = 60'000;

    while (ctx->discoveryRunning) {
        std::map<std::string, std::string> names;
        std::map<std::string, std::string> byId;
        std::map<std::string, std::string> idLabel;
        for (const MdnsReceiver& receiver : BrowseReceivers(kBrowseMs)) {
            const std::string label = ReceiverLabel(receiver);
            names[receiver.address] = label;
            if (!receiver.id.empty()) {
                byId[receiver.id] = receiver.address;
                idLabel[receiver.id] = label;
            }
        }

        if (!names.empty() || !byId.empty()) {
            std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
            // Merged, not replaced: an iPad that is asleep doesn't answer, and
            // dropping its name would make the menu flip back to a raw address.
            for (auto& name : names)
                ctx->discovered[name.first] = name.second;
            for (auto& entry : byId)
                ctx->discoveredById[entry.first] = entry.second;
            for (auto& entry : idLabel)
                ctx->discoveredIdLabel[entry.first] = entry.second;
        }

        for (int waited = 0; waited < kPauseMs && ctx->discoveryRunning; waited += 250)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

bool AnyStreaming(const TrayContext* ctx)
{
    for (const auto& app : ctx->apps)
        if (app->GetState() == SenderApp::State::Streaming)
            return true;
    return false;
}

bool AnyRunning(const TrayContext* ctx)
{
    for (const auto& app : ctx->apps)
        if (app->IsRunning())
            return true;
    return false;
}

HICON MakeStatusIcon(COLORREF dotColor)
{
    constexpr int S = 32;
    HDC screen = GetDC(nullptr);
    HDC colorDC = CreateCompatibleDC(screen);
    HDC maskDC = CreateCompatibleDC(screen);
    HBITMAP colorBmp = CreateCompatibleBitmap(screen, S, S);
    HBITMAP maskBmp = CreateBitmap(S, S, 1, 1, nullptr);
    ReleaseDC(nullptr, screen);

    HBITMAP oldColor = static_cast<HBITMAP>(SelectObject(colorDC, colorBmp));
    HBITMAP oldMask = static_cast<HBITMAP>(SelectObject(maskDC, maskBmp));

    RECT rc{0, 0, S, S};
    FillRect(colorDC, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    FillRect(maskDC, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    auto opaqueMask = [&] {
        SelectObject(maskDC, GetStockObject(BLACK_BRUSH));
        SelectObject(maskDC, GetStockObject(BLACK_PEN));
    };

    HBRUSH bodyBrush = CreateSolidBrush(RGB(55, 65, 90));
    HPEN edgePen = CreatePen(PS_SOLID, 1, RGB(20, 24, 34));
    SelectObject(colorDC, bodyBrush);
    SelectObject(colorDC, edgePen);
    RoundRect(colorDC, 2, 5, 30, 23, 5, 5);
    Rectangle(colorDC, 13, 22, 19, 27);
    Rectangle(colorDC, 8, 27, 24, 30);
    opaqueMask();
    RoundRect(maskDC, 2, 5, 30, 23, 5, 5);
    Rectangle(maskDC, 13, 22, 19, 27);
    Rectangle(maskDC, 8, 27, 24, 30);

    HBRUSH innerBrush = CreateSolidBrush(RGB(120, 150, 195));
    SelectObject(colorDC, innerBrush);
    SelectObject(colorDC, static_cast<HPEN>(GetStockObject(NULL_PEN)));
    RoundRect(colorDC, 5, 8, 27, 20, 3, 3);

    HBRUSH dotBrush = CreateSolidBrush(dotColor);
    HPEN dotPen = CreatePen(PS_SOLID, 1, RGB(245, 245, 245));
    SelectObject(colorDC, dotBrush);
    SelectObject(colorDC, dotPen);
    Ellipse(colorDC, 0, 0, 14, 14);
    opaqueMask();
    Ellipse(maskDC, 0, 0, 14, 14);

    SelectObject(colorDC, oldColor);
    SelectObject(maskDC, oldMask);
    DeleteDC(colorDC);
    DeleteDC(maskDC);
    DeleteObject(bodyBrush);
    DeleteObject(edgePen);
    DeleteObject(innerBrush);
    DeleteObject(dotBrush);
    DeleteObject(dotPen);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = colorBmp;
    ii.hbmMask = maskBmp;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon;
}

HICON PickIcon(TrayContext* ctx)
{
    if (ctx->cfg.devices.empty())
        return ctx->iconGrey;
    if (AnyStreaming(ctx))
        return ctx->iconGreen;
    return ctx->iconRed;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// A configured device is "<address>[ <name>]": everything up to the first space
// is the address, the rest is the label; without one the address is the label.
struct DeviceEntry {
    std::string address;
    std::string label;
};

DeviceEntry SplitDevice(const std::string& entry)
{
    size_t space = entry.find_first_of(" \t");
    if (space == std::string::npos)
        return {entry, entry};

    std::string address = entry.substr(0, space);
    size_t nameStart = entry.find_first_not_of(" \t", space);
    if (nameStart == std::string::npos)
        return {address, address};

    size_t nameEnd = entry.find_last_not_of(" \t");
    return {address, entry.substr(nameStart, nameEnd - nameStart + 1)};
}

bool IsLoopbackHost(const std::string& host)
{
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

// USB-discovery leftovers used 127.0.0.1 + serial as the Your iPads row.
// Nearby (Wi-Fi) adds always store a LAN IP; USB is only a later transport.
bool IsUsbOnlyPlaceholder(const std::string& device)
{
    return IsLoopbackHost(SplitDevice(device).address);
}

void PruneUsbOnlyPlaceholders(std::vector<std::string>& devices,
                              std::map<std::string, std::string>* pendingIds)
{
    std::vector<std::string> kept;
    kept.reserve(devices.size());
    for (const std::string& device : devices) {
        if (IsUsbOnlyPlaceholder(device)) {
            if (pendingIds)
                pendingIds->erase(SplitDevice(device).address);
            continue;
        }
        kept.push_back(device);
    }
    devices = std::move(kept);
}

bool IsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION el{};
    DWORD size = sizeof(el);
    bool ok = GetTokenInformation(token, TokenElevation, &el, sizeof(el), &size) != 0;
    CloseHandle(token);
    return ok && el.TokenIsElevated;
}

const wchar_t* const kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* const kRunValue = L"opendisplay-win";

bool IsAutostartEnabled()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    bool exists = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

void SetAutostart(bool enable)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0) {
            std::wstring val = L"\"" + std::wstring(exe) + L"\"";
            RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()),
                           static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// Prefer the live address from mDNS when we have a stored TXT id.
std::string ResolveDeviceAddress(const TrayContext* ctx, size_t index)
{
    DeviceEntry entry = SplitDevice(ctx->cfg.devices[index]);
    if (index < ctx->cfg.deviceIds.size() && !ctx->cfg.deviceIds[index].empty()) {
        std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
        auto it = ctx->discoveredById.find(ctx->cfg.deviceIds[index]);
        if (it != ctx->discoveredById.end() && !it->second.empty())
            return it->second;
    }
    return entry.address;
}

std::wstring DeviceLabel(const TrayContext* ctx, size_t index)
{
    DeviceEntry entry = SplitDevice(ctx->cfg.devices[index]);
    if (entry.label != entry.address)
        return Widen(entry.label);

    if (index < ctx->cfg.deviceIds.size() && !ctx->cfg.deviceIds[index].empty()) {
        std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
        auto byId = ctx->discoveredIdLabel.find(ctx->cfg.deviceIds[index]);
        if (byId != ctx->discoveredIdLabel.end())
            return Widen(byId->second);
    }

    std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
    auto discovered = ctx->discovered.find(entry.address);
    return Widen(discovered != ctx->discovered.end() ? discovered->second : entry.address);
}

std::wstring DeviceStatusText(const TrayContext* ctx, size_t index)
{
    std::wstring text = DeviceLabel(ctx, index);
    const SenderApp& app = *ctx->apps[index];
    switch (app.GetState()) {
        case SenderApp::State::Streaming: {
            const wchar_t* link = L"";
            switch (app.GetTransport()) {
                case SenderApp::Transport::Usb:
                    link = L" (USB)";
                    break;
                case SenderApp::Transport::Wifi:
                    link = L" (Wi-Fi)";
                    break;
                default:
                    break;
            }
            return text + L"  Connected" + link;
        }
        case SenderApp::State::Connecting:
            return text + L"  Connecting...";
        case SenderApp::State::Blocked:
            return text + L"  Not connected";
        default:
            return text + L"  Not connected";
    }
}

void UpdateStatus(TrayContext* ctx)
{
    std::wstring tip = L"OpenDisplay";
    if (ctx->cfg.devices.empty()) {
        tip += L" - no iPad configured";
    } else {
        for (size_t i = 0; i < ctx->cfg.devices.size(); ++i)
            tip += (i == 0 ? L" - " : L" | ") + DeviceStatusText(ctx, i);
    }

    ctx->nid.hIcon = PickIcon(ctx);
    wcsncpy_s(ctx->nid.szTip, tip.c_str(), _TRUNCATE);
    ctx->nid.uFlags = NIF_ICON | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &ctx->nid);
}

void ApplyAndRestart(TrayContext* ctx)
{
    RebuildSenders(ctx);
    for (size_t i = 0; i < ctx->apps.size(); ++i)
        ctx->apps[i]->Start(ResolveDeviceAddress(ctx, i), ctx->cfg.port);
    UpdateStatus(ctx);
}

void FillPictureCombo(HWND dlg)
{
    HWND cb = GetDlgItem(dlg, IDC_PICTURE_PRESET);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Faster response"));
    SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Balanced"));
    SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Sharper"));
}

void SelectPictureCombo(HWND dlg, const std::string& preset)
{
    int idx = 0;
    if (preset == "balanced")
        idx = 1;
    else if (preset == "quality")
        idx = 2;
    SendDlgItemMessageW(dlg, IDC_PICTURE_PRESET, CB_SETCURSEL, idx, 0);
}

std::string ReadPictureCombo(HWND dlg)
{
    LRESULT i = SendDlgItemMessageW(dlg, IDC_PICTURE_PRESET, CB_GETCURSEL, 0, 0);
    if (i == 1)
        return "balanced";
    if (i == 2)
        return "quality";
    return "speed";
}

void FillEncodeCombos(HWND dlg)
{
    auto fillMode = [&](int id) {
        HWND cb = GetDlgItem(dlg, id);
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dynamic"));
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Fixed"));
    };
    auto fillPreset = [&](int id) {
        HWND cb = GetDlgItem(dlg, id);
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Speed"));
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Balanced"));
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Quality"));
    };
    fillMode(IDC_USB_ENCODE_MODE);
    fillMode(IDC_WIFI_ENCODE_MODE);
    fillPreset(IDC_USB_ENCODE_PRESET);
    fillPreset(IDC_WIFI_ENCODE_PRESET);
}

void SelectEncodeCombos(HWND dlg, const Config& cfg)
{
    auto selMode = [&](int id, const std::string& mode) {
        SendDlgItemMessageW(dlg, id, CB_SETCURSEL, mode == "fixed" ? 1 : 0, 0);
    };
    auto selPreset = [&](int id, const std::string& preset) {
        int idx = 0;
        if (preset == "balanced")
            idx = 1;
        else if (preset == "quality")
            idx = 2;
        SendDlgItemMessageW(dlg, id, CB_SETCURSEL, idx, 0);
    };
    selMode(IDC_USB_ENCODE_MODE, cfg.usbEncodeMode);
    selMode(IDC_WIFI_ENCODE_MODE, cfg.wifiEncodeMode);
    selPreset(IDC_USB_ENCODE_PRESET, cfg.usbEncodePreset);
    selPreset(IDC_WIFI_ENCODE_PRESET, cfg.wifiEncodePreset);
}

void ReadEncodeCombos(HWND dlg, Config& cfg)
{
    auto modeOf = [&](int id) -> std::string {
        return SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0) == 1 ? "fixed" : "dynamic";
    };
    auto presetOf = [&](int id) -> std::string {
        LRESULT i = SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0);
        if (i == 1)
            return "balanced";
        if (i == 2)
            return "quality";
        return "speed";
    };
    cfg.usbEncodeMode = modeOf(IDC_USB_ENCODE_MODE);
    cfg.wifiEncodeMode = modeOf(IDC_WIFI_ENCODE_MODE);
    cfg.usbEncodePreset = presetOf(IDC_USB_ENCODE_PRESET);
    cfg.wifiEncodePreset = presetOf(IDC_WIFI_ENCODE_PRESET);
}

void SyncDevicesEdit(HWND dlg, const SettingsDlgState* state);
void RefreshSavedList(HWND dlg, SettingsDlgState* state);
void ReadDevicesEdit(HWND dlg, SettingsDlgState* state);

const int kGeneralControlIds[] = {
    IDC_LEAD_HINT, IDC_NEARBY_GROUP, IDC_NEARBY, IDC_REFRESH_NEARBY, IDC_NEARBY_HINT, IDC_ADD_NEARBY,
    IDC_REMOVE_SAVED, IDC_SAVED_GROUP, IDC_SAVED, IDC_CONNECTION_GROUP, IDC_AUTORECONNECT,
    IDC_USB_WIFI_FAILOVER, IDC_AUTOSTART, IDC_QUALITY_GROUP, IDC_QUALITY_LABEL, IDC_PICTURE_PRESET,
};

const int kAdvancedControlIds[] = {
    IDC_ADV_MANUAL_GROUP, IDC_ADV_MANUAL_HINT, IDC_DEVICES, IDC_ADV_MANUAL_EXAMPLE,
    IDC_ADV_PORT_LABEL, IDC_PORT, IDC_SEPARATE_USB_WIFI,
    IDC_ADV_USB_LABEL, IDC_ADV_USB_MODE_LABEL, IDC_USB_ENCODE_MODE,
    IDC_ADV_USB_PRESET_LABEL, IDC_USB_ENCODE_PRESET,
    IDC_ADV_WIFI_LABEL, IDC_ADV_WIFI_MODE_LABEL, IDC_WIFI_ENCODE_MODE,
    IDC_ADV_WIFI_PRESET_LABEL, IDC_WIFI_ENCODE_PRESET,
    IDC_RESTART_ADMIN, IDC_ADMIN_NOTE,
};

void UpdateSeparateEncodeEnable(HWND dlg, bool advancedVisible)
{
    // Separate USB/Wi-Fi combos only useful when Advanced is showing and that checkbox is on.
    const bool separate = IsDlgButtonChecked(dlg, IDC_SEPARATE_USB_WIFI) == BST_CHECKED;
    const BOOL enableSeparate = (advancedVisible && separate) ? TRUE : FALSE;
    for (int id : {IDC_USB_ENCODE_MODE, IDC_USB_ENCODE_PRESET, IDC_WIFI_ENCODE_MODE, IDC_WIFI_ENCODE_PRESET,
                   IDC_ADV_USB_LABEL, IDC_ADV_USB_MODE_LABEL, IDC_ADV_USB_PRESET_LABEL, IDC_ADV_WIFI_LABEL,
                   IDC_ADV_WIFI_MODE_LABEL, IDC_ADV_WIFI_PRESET_LABEL})
        EnableWindow(GetDlgItem(dlg, id), enableSeparate);
}

void ShowSettingsTab(HWND dlg, SettingsDlgState* state, int tab)
{
    // Sync devices list <-> advanced multiline when leaving/entering Advanced.
    if (state->settingsTab == 1 && tab != 1) {
        ReadDevicesEdit(dlg, state);
        RefreshSavedList(dlg, state);
    } else if (tab == 1 && state->settingsTab != 1) {
        SyncDevicesEdit(dlg, state);
    }

    state->settingsTab = tab;
    const bool general = (tab == 0);
    for (int id : kGeneralControlIds)
        ShowWindow(GetDlgItem(dlg, id), general ? SW_SHOW : SW_HIDE);
    for (int id : kAdvancedControlIds)
        ShowWindow(GetDlgItem(dlg, id), general ? SW_HIDE : SW_SHOW);
    UpdateSeparateEncodeEnable(dlg, !general);
}

void InitSettingsTabs(HWND dlg)
{
    HWND tabs = GetDlgItem(dlg, IDC_SETTINGS_TABS);
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(L"General");
    TabCtrl_InsertItem(tabs, 0, &item);
    item.pszText = const_cast<wchar_t*>(L"Advanced");
    TabCtrl_InsertItem(tabs, 1, &item);
    TabCtrl_SetCurSel(tabs, 0);
}

// Enlarge the tab control to the band above OK/Cancel, then place General /
// Advanced siblings inside TCM_ADJUSTRECT's display rect so content sits in
// the tab page frame (not below a short strip).
void LayoutSettingsTabPages(HWND dlg)
{
    HWND tabs = GetDlgItem(dlg, IDC_SETTINGS_TABS);
    HWND ok = GetDlgItem(dlg, IDOK);
    if (!tabs || !ok)
        return;

    RECT dlgClient{};
    GetClientRect(dlg, &dlgClient);

    RECT okRect{};
    GetWindowRect(ok, &okRect);
    MapWindowPoints(HWND_DESKTOP, dlg, reinterpret_cast<POINT*>(&okRect), 2);

    // Match resources.rc margins: 8 DU inset; small gap above OK/Cancel.
    RECT margin{8, 6, 8, 8};
    MapDialogRect(dlg, &margin);
    const int gap = margin.bottom; // 8 DU -> px
    const int left = margin.left;
    const int top = margin.top;
    const int right = dlgClient.right - margin.right;
    const int bottom = okRect.top - gap;
    if (right <= left || bottom <= top)
        return;

    SetWindowPos(tabs, HWND_BOTTOM, left, top, right - left, bottom - top,
                 SWP_NOACTIVATE);

    RECT display{};
    GetClientRect(tabs, &display);
    TabCtrl_AdjustRect(tabs, FALSE, &display);
    MapWindowPoints(tabs, dlg, reinterpret_cast<POINT*>(&display), 2);

    // Design page origin / extent from resources.rc (dialog units).
    RECT design{16, 34, 256, 264};
    MapDialogRect(dlg, &design);

    const int pad = 4;
    const int dx = (display.left + pad) - design.left;
    const int dy = (display.top + pad) - design.top;

    auto moveId = [&](int id) {
        HWND ctl = GetDlgItem(dlg, id);
        if (!ctl)
            return;
        RECT rc{};
        GetWindowRect(ctl, &rc);
        MapWindowPoints(HWND_DESKTOP, dlg, reinterpret_cast<POINT*>(&rc), 2);
        OffsetRect(&rc, dx, dy);
        SetWindowPos(ctl, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    };

    for (int id : kGeneralControlIds)
        moveId(id);
    for (int id : kAdvancedControlIds)
        moveId(id);
}


void SetNearbyHint(HWND dlg, bool empty)
{
    // Short one-liner next to Refresh; Add > is enough when the list has rows.
    const wchar_t* text = empty ? L"None found on this Wi-Fi." : L"";
    SetDlgItemTextW(dlg, IDC_NEARBY_HINT, text);
}

void SyncDevicesEdit(HWND dlg, const SettingsDlgState* state)
{
    std::wstring list;
    for (const std::string& device : state->devices)
        list += Widen(device) + L"\r\n";
    SetDlgItemTextW(dlg, IDC_DEVICES, list.c_str());
}

void RefreshSavedList(HWND dlg, SettingsDlgState* state)
{
    PruneUsbOnlyPlaceholders(state->devices, &state->pendingIds);

    HWND list = GetDlgItem(dlg, IDC_SAVED);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const std::string& device : state->devices) {
        DeviceEntry entry = SplitDevice(device);
        std::string label = entry.label;

        // Prefer live Wi-Fi / mDNS host name when we know the device id.
        auto idIt = state->pendingIds.find(entry.address);
        if (idIt != state->pendingIds.end() && !idIt->second.empty() && state->tray) {
            std::lock_guard<std::mutex> lock(state->tray->discoveredMutex);
            auto byId = state->tray->discoveredIdLabel.find(idIt->second);
            if (byId != state->tray->discoveredIdLabel.end() && !byId->second.empty())
                label = byId->second;
        } else if (state->tray) {
            std::lock_guard<std::mutex> lock(state->tray->discoveredMutex);
            auto byAddr = state->tray->discovered.find(entry.address);
            if (byAddr != state->tray->discovered.end() && !byAddr->second.empty())
                label = byAddr->second;
        }

        std::wstring line = Widen(label);
        if (label != entry.address) {
            line += L"  (";
            line += Widen(entry.address);
            line += L")";
        }
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
}

// Pull the advanced multiline edit back into state->devices (manual add / rename).
void ReadDevicesEdit(HWND dlg, SettingsDlgState* state)
{
    wchar_t buf[4096] = {};
    GetDlgItemTextW(dlg, IDC_DEVICES, buf, static_cast<int>(std::size(buf)));
    state->devices.clear();
    std::wstring text(buf);
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", pos);
        if (end == std::wstring::npos)
            end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        size_t first = line.find_first_not_of(L" \t");
        size_t last = line.find_last_not_of(L" \t");
        if (first != std::wstring::npos) {
            std::string device = Narrow(line.substr(first, last - first + 1));
            if (IsUsbOnlyPlaceholder(device))
                continue; // never re-admit USB-only leftover rows
            std::string address = SplitDevice(device).address;
            bool known = false;
            for (const std::string& existing : state->devices)
                known = known || SplitDevice(existing).address == address;
            if (!known)
                state->devices.push_back(device);
        }
        pos = end + 1;
    }
}

NearbyEntry WifiOnlyEntry(const MdnsReceiver& receiver)
{
    NearbyEntry e;
    e.address = receiver.address;
    e.label = ReceiverLabel(receiver);
    e.id = receiver.id;
    e.viaWifi = true;
    return e;
}

// Nearby = Wi-Fi (mDNS) only. USB devices still connect via Your iPads / usbmux;
// they are not listed here for discovery.
std::vector<NearbyEntry> BuildNearbyEntries(const std::vector<MdnsReceiver>& wifi)
{
    std::vector<NearbyEntry> out;
    out.reserve(wifi.size());
    for (const MdnsReceiver& r : wifi)
        out.push_back(WifiOnlyEntry(r));
    return out;
}

void RefreshNearbyList(HWND dlg, SettingsDlgState* state)
{
    const std::vector<MdnsReceiver> wifi = BrowseReceivers(800);
    state->nearby = BuildNearbyEntries(wifi);

    HWND list = GetDlgItem(dlg, IDC_NEARBY);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const NearbyEntry& entry : state->nearby) {
        std::wstring line = Widen(entry.label);
        line += L"  (";
        line += Widen(entry.address);
        line += L")";
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
    SetNearbyHint(dlg, state->nearby.empty());
}

void OnRefreshNearby(HWND dlg, SettingsDlgState* state)
{
    SetDlgItemTextW(dlg, IDC_NEARBY_HINT, L"Looking...");
    UpdateWindow(GetDlgItem(dlg, IDC_NEARBY_HINT));
    RefreshNearbyList(dlg, state);
}

void AddSelectedNearby(HWND dlg, SettingsDlgState* state)
{
    HWND list = GetDlgItem(dlg, IDC_NEARBY);
    LRESULT sel = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel < 0 || static_cast<size_t>(sel) >= state->nearby.size())
        return;

    const NearbyEntry& receiver = state->nearby[static_cast<size_t>(sel)];
    if (IsLoopbackHost(receiver.address) || !receiver.viaWifi)
        return; // Your iPads only accepts Wi-Fi Nearby discoveries
    for (size_t i = 0; i < state->devices.size(); ++i) {
        if (SplitDevice(state->devices[i]).address == receiver.address)
            return; // already listed
        if (!receiver.id.empty()) {
            auto it = state->pendingIds.find(SplitDevice(state->devices[i]).address);
            if (it != state->pendingIds.end() && it->second == receiver.id)
                return;
        }
    }

    std::string entry = receiver.address;
    if (!receiver.label.empty() && receiver.label != receiver.address)
        entry += " " + receiver.label;

    state->devices.push_back(entry);
    if (!receiver.id.empty())
        state->pendingIds[receiver.address] = receiver.id;
    // USB-only Add: remember serial so a later hello can bind without re-guessing.
    if (receiver.viaUsb && !receiver.usbSerial.empty() && !receiver.id.empty() && state->cfg)
        state->cfg->RememberUsbSerialId(receiver.usbSerial, receiver.id);
    RefreshSavedList(dlg, state);
    SyncDevicesEdit(dlg, state);
}

void RemoveSelectedSaved(HWND dlg, SettingsDlgState* state)
{
    HWND list = GetDlgItem(dlg, IDC_SAVED);
    LRESULT sel = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel < 0 || static_cast<size_t>(sel) >= state->devices.size())
        return;
    state->devices.erase(state->devices.begin() + static_cast<size_t>(sel));
    RefreshSavedList(dlg, state);
    SyncDevicesEdit(dlg, state);
}

bool RelaunchElevated(HWND hwnd, TrayContext* ctx)
{
    for (auto& app : ctx->apps)
        app->Stop();
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.nShow = SW_NORMAL;
    if (ShellExecuteExW(&sei)) {
        DestroyWindow(hwnd);
        return true;
    }
    ApplyAndRestart(ctx);
    return false;
}

INT_PTR CALLBACK SettingsDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_INITDIALOG: {
            auto* state = reinterpret_cast<SettingsDlgState*>(lParam);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, static_cast<LONG_PTR>(lParam));
            Config* cfg = state->cfg;

            state->devices = cfg->devices;
            PruneUsbOnlyPlaceholders(state->devices, &state->pendingIds);

            // Seed pending ids from the saved config so hand-edited nicknames keep ids.
            cfg->deviceIds.resize(cfg->devices.size());
            for (size_t i = 0; i < cfg->devices.size(); ++i) {
                if (IsUsbOnlyPlaceholder(cfg->devices[i]))
                    continue;
                if (!cfg->deviceIds[i].empty())
                    state->pendingIds[SplitDevice(cfg->devices[i]).address] = cfg->deviceIds[i];
            }

            SyncDevicesEdit(dlg, state);
            RefreshSavedList(dlg, state);
            SetDlgItemInt(dlg, IDC_PORT, cfg->port, FALSE);
            CheckDlgButton(dlg, IDC_AUTORECONNECT, cfg->autoReconnect ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_USB_WIFI_FAILOVER, cfg->usbWifiFailover ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_AUTOSTART, IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_SEPARATE_USB_WIFI, cfg->separateUsbWifiPicture ? BST_CHECKED : BST_UNCHECKED);

            FillPictureCombo(dlg);
            SelectPictureCombo(dlg, cfg->picturePreset);
            FillEncodeCombos(dlg);
            SelectEncodeCombos(dlg, *cfg);

            InitSettingsTabs(dlg);
            LayoutSettingsTabPages(dlg);
            state->settingsTab = -1; // force first ShowSettingsTab to treat as switch-in
            ShowSettingsTab(dlg, state, 0);
            RefreshNearbyList(dlg, state);

            if (IsElevated())
                EnableWindow(GetDlgItem(dlg, IDC_RESTART_ADMIN), FALSE);
            return TRUE;
        }
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<LPNMHDR>(lParam);
            if (hdr && hdr->idFrom == IDC_SETTINGS_TABS && hdr->code == TCN_SELCHANGE) {
                auto* state = reinterpret_cast<SettingsDlgState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
                const int tab = TabCtrl_GetCurSel(hdr->hwndFrom);
                ShowSettingsTab(dlg, state, tab);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            auto* state = reinterpret_cast<SettingsDlgState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
            const int id = LOWORD(wParam);
            if (id == IDC_SEPARATE_USB_WIFI) {
                UpdateSeparateEncodeEnable(dlg, state->settingsTab == 1);
                return TRUE;
            }
            if (id == IDC_REFRESH_NEARBY) {
                OnRefreshNearby(dlg, state);
                return TRUE;
            }
            if (id == IDC_ADD_NEARBY) {
                AddSelectedNearby(dlg, state);
                return TRUE;
            }
            if (id == IDC_NEARBY && HIWORD(wParam) == LBN_DBLCLK) {
                AddSelectedNearby(dlg, state);
                return TRUE;
            }
            if (id == IDC_REMOVE_SAVED) {
                RemoveSelectedSaved(dlg, state);
                return TRUE;
            }
            if (id == IDC_RESTART_ADMIN)
                state->restartAdminRequested = true;
            if (id == IDOK || id == IDC_RESTART_ADMIN) {
                Config* cfg = state->cfg;

                // Multiline may be hidden on General; GetDlgItemText still works.
                ReadDevicesEdit(dlg, state);
                PruneUsbOnlyPlaceholders(state->devices, &state->pendingIds);

                cfg->devices = state->devices;
                std::vector<std::string> ids;
                ids.reserve(cfg->devices.size());
                for (const std::string& device : cfg->devices) {
                    auto it = state->pendingIds.find(SplitDevice(device).address);
                    ids.push_back(it != state->pendingIds.end() ? it->second : std::string{});
                }
                cfg->deviceIds = std::move(ids);

                BOOL ok = FALSE;
                UINT port = GetDlgItemInt(dlg, IDC_PORT, &ok, FALSE);
                if (ok && port > 0 && port <= 65535)
                    cfg->port = static_cast<uint16_t>(port);
                cfg->autoReconnect = IsDlgButtonChecked(dlg, IDC_AUTORECONNECT) == BST_CHECKED;
                cfg->usbWifiFailover = IsDlgButtonChecked(dlg, IDC_USB_WIFI_FAILOVER) == BST_CHECKED;
                SetAutostart(IsDlgButtonChecked(dlg, IDC_AUTOSTART) == BST_CHECKED);
                cfg->picturePreset = ReadPictureCombo(dlg);
                cfg->separateUsbWifiPicture = IsDlgButtonChecked(dlg, IDC_SEPARATE_USB_WIFI) == BST_CHECKED;
                ReadEncodeCombos(dlg, *cfg);
                if (!cfg->separateUsbWifiPicture)
                    cfg->SyncTransportsFromPicturePreset();
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

void ShowContextMenu(HWND hwnd, TrayContext* ctx)
{
    HMENU menu = CreatePopupMenu();

    bool several = ctx->cfg.devices.size() > 1;
    for (size_t i = 0; i < ctx->cfg.devices.size(); ++i) {
        UINT flags = MF_STRING | (ctx->apps[i]->GetState() != SenderApp::State::Idle ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, IDM_DEVICE_FIRST + static_cast<UINT>(i), DeviceStatusText(ctx, i).c_str());
    }
    if (!ctx->cfg.devices.empty())
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (AnyRunning(ctx)) {
        AppendMenuW(menu, MF_STRING, IDM_DISCONNECT, several ? L"Disconnect all" : L"Disconnect");
    } else {
        AppendMenuW(menu, MF_STRING | (ctx->cfg.devices.empty() ? MF_GRAYED : 0), IDM_CONNECT,
                    several ? L"Connect all" : L"Connect");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void OpenSettingsAndApply(HWND hwnd, TrayContext* ctx)
{
    std::vector<std::string> oldDevices = ctx->cfg.devices;
    std::vector<std::string> oldIds = ctx->cfg.deviceIds;
    uint16_t oldPort = ctx->cfg.port;
    const std::string oldUsbMode = ctx->cfg.usbEncodeMode;
    const std::string oldUsbPreset = ctx->cfg.usbEncodePreset;
    const std::string oldWifiMode = ctx->cfg.wifiEncodeMode;
    const std::string oldWifiPreset = ctx->cfg.wifiEncodePreset;
    const std::string oldPicture = ctx->cfg.picturePreset;
    const bool oldSeparate = ctx->cfg.separateUsbWifiPicture;
    const bool oldFailover = ctx->cfg.usbWifiFailover;

    SettingsDlgState state;
    state.tray = ctx;
    state.cfg = &ctx->cfg;

    if (DialogBoxParamW(ctx->hInstance, MAKEINTRESOURCEW(IDD_SETTINGS), hwnd, SettingsDlgProc,
                        reinterpret_cast<LPARAM>(&state)) != IDOK)
        return;

    if (state.restartAdminRequested) {
        ctx->cfg.Save();
        RelaunchElevated(hwnd, ctx);
        return;
    }

    ctx->cfg.Save();
    const bool targetsChanged =
        ctx->cfg.devices != oldDevices || ctx->cfg.deviceIds != oldIds || ctx->cfg.port != oldPort;
    const bool encodeChanged =
        ctx->cfg.usbEncodeMode != oldUsbMode || ctx->cfg.usbEncodePreset != oldUsbPreset ||
        ctx->cfg.wifiEncodeMode != oldWifiMode || ctx->cfg.wifiEncodePreset != oldWifiPreset ||
        ctx->cfg.picturePreset != oldPicture || ctx->cfg.separateUsbWifiPicture != oldSeparate;
    // Failover is read each connect; no restart required. Targets/encode still are.
    (void)oldFailover;
    if (targetsChanged || encodeChanged)
        ApplyAndRestart(ctx);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<TrayContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_APP_NOTIFY: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
            if (text && ctx) {
                NOTIFYICONDATAW info = ctx->nid;
                info.uFlags = NIF_INFO;
                info.dwInfoFlags = NIIF_INFO;
                wcsncpy_s(info.szInfoTitle, L"OpenDisplay", _TRUNCATE);
                wcsncpy_s(info.szInfo, text->c_str(), _TRUNCATE);
                Shell_NotifyIconW(NIM_MODIFY, &info);
            }
            return 0;
        }

        case WM_APP_TRAY:
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
                ShowContextMenu(hwnd, ctx);
            else if (LOWORD(lParam) == WM_LBUTTONDBLCLK)
                OpenSettingsAndApply(hwnd, ctx);
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) >= IDM_DEVICE_FIRST && LOWORD(wParam) < IDM_DEVICE_FIRST + ctx->apps.size()) {
                size_t index = LOWORD(wParam) - IDM_DEVICE_FIRST;
                SenderApp& app = *ctx->apps[index];
                if (app.IsRunning())
                    app.Stop();
                else
                    app.Start(ResolveDeviceAddress(ctx, index), ctx->cfg.port);
                UpdateStatus(ctx);
                return 0;
            }

            switch (LOWORD(wParam)) {
                case IDM_CONNECT:
                    for (size_t i = 0; i < ctx->apps.size(); ++i)
                        ctx->apps[i]->Start(ResolveDeviceAddress(ctx, i), ctx->cfg.port);
                    UpdateStatus(ctx);
                    return 0;
                case IDM_DISCONNECT:
                    for (auto& app : ctx->apps)
                        app->Stop();
                    UpdateStatus(ctx);
                    return 0;
                case IDM_SETTINGS:
                    OpenSettingsAndApply(hwnd, ctx);
                    return 0;
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
            }
            return 0;

        case WM_TIMER:
            if (wParam == kStatusTimerId)
                UpdateStatus(ctx);
            else if (wParam == kDisplayChangeTimerId) {
                KillTimer(hwnd, kDisplayChangeTimerId);
                // User layout change while the MTT head is attached: persist it
                // instead of reverting. No-op if MTT is inactive/absent or the
                // change was one of our own applies.
                (void)SaveMttDisplayTopologyIfUserChange("display");
            }
            return 0;

        case WM_DISPLAYCHANGE:
            SetTimer(hwnd, kDisplayChangeTimerId, kDisplayChangeDebounceMs, nullptr);
            return 0;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wParam) {
                // Logoff / shutdown / restart: same teardown as Quit, so the
                // MTT head does not survive into the next session.
                Logf("mtt", "session ending - tearing down\n");
                BeginMttShutdown();
                KillTimer(hwnd, kStatusTimerId);
                KillTimer(hwnd, kDisplayChangeTimerId);
                for (auto& app : ctx->apps)
                    app->Stop();
                (void)ShutdownMttVddForExit("exit");
            }
            return 0;

        case WM_DESTROY:
            SetMttUserNotifier(nullptr);
            KillTimer(hwnd, kStatusTimerId);
            KillTimer(hwnd, kDisplayChangeTimerId);
            Shell_NotifyIconW(NIM_DELETE, &ctx->nid);
            // Quit: block re-attach from any racing pipeline rebuild, stop the
            // senders (a streaming one tears down as user-initiated), then tear
            // down unconditionally - covers idle/connecting senders and a
            // pending link-loss grace, which previously left the head behind.
            BeginMttShutdown();
            for (auto& app : ctx->apps)
                app->Stop();
            (void)ShutdownMttVddForExit("exit");
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int RunTray(HINSTANCE hInstance)
{
    // One tray per session. A second tray would adopt / tear down the MTT head
    // the first one is streaming to. Wait briefly so a self-relaunch
    // (RelaunchElevated) can take over once the old instance has exited.
    HANDLE trayMutex = CreateMutexW(nullptr, FALSE, L"Local\\opendisplay-win-tray");
    if (trayMutex == nullptr) {
        Logf("tray", "another opendisplay-win tray is running (mutex unavailable) - exiting\n");
        return 0;
    }
    const DWORD w = WaitForSingleObject(trayMutex, 10000);
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
        Logf("tray", "another opendisplay-win tray is running - exiting\n");
        CloseHandle(trayMutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);

    TrayContext ctx;
    ctx.hInstance = hInstance;
    ctx.cfg = Config::Load();
    RebuildSenders(&ctx);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    // Hidden top-level window (not HWND_MESSAGE): message-only windows never
    // receive broadcasts, and we need WM_ENDSESSION (teardown on logoff /
    // shutdown) and WM_DISPLAYCHANGE (save user layout changes).
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWndClass, L"opendisplay-win", WS_POPUP, 0, 0, 0, 0, nullptr,
                                nullptr, hInstance, nullptr);
    if (hwnd == nullptr)
        return 1;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ctx));

    ctx.iconGreen = MakeStatusIcon(RGB(40, 200, 80));
    ctx.iconRed = MakeStatusIcon(RGB(225, 65, 55));
    ctx.iconGrey = MakeStatusIcon(RGB(150, 150, 150));

    ctx.nid.cbSize = sizeof(ctx.nid);
    ctx.nid.hWnd = hwnd;
    ctx.nid.uID = 1;
    ctx.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    ctx.nid.uCallbackMessage = WM_APP_TRAY;
    ctx.nid.hIcon = PickIcon(&ctx);
    wcsncpy_s(ctx.nid.szTip, L"OpenDisplay", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &ctx.nid);

    SetTimer(hwnd, kStatusTimerId, kStatusTimerMs, nullptr);

    ctx.discoveryRunning = true;
    ctx.discovery = std::thread([&ctx] { RunDiscovery(&ctx); });

    // One-time notice right before an EDID-change UAC prompt (sender thread ->
    // tray thread via PostMessage).
    SetMttUserNotifier([hwnd](const std::wstring& text) {
        auto* copy = new std::wstring(text);
        if (!PostMessageW(hwnd, WM_APP_NOTIFY, 0, reinterpret_cast<LPARAM>(copy)))
            delete copy;
    });

    AdoptStaleMttHeadAtStartup("mtt", kStaleMttHeadGraceMs);

    if (ctx.cfg.autoReconnect)
        for (size_t i = 0; i < ctx.apps.size(); ++i)
            ctx.apps[i]->Start(ResolveDeviceAddress(&ctx, i), ctx.cfg.port);
    UpdateStatus(&ctx);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ctx.discoveryRunning = false;
    if (ctx.discovery.joinable())
        ctx.discovery.join();

    // Normal process exit safety net (idempotent after WM_DESTROY).
    for (auto& app : ctx.apps)
        app->Stop();
    (void)ShutdownMttVddForExit("exit");

    DestroyIcon(ctx.iconGreen);
    DestroyIcon(ctx.iconRed);
    DestroyIcon(ctx.iconGrey);
    return 0;
}

} // namespace od
