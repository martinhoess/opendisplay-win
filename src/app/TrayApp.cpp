#include "app/TrayApp.h"

#include "app/Config.h"
#include "app/SenderApp.h"
#include "app/resources.h"
#include "net/Mdns.h"

#include <shellapi.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace od {

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT kStatusTimerId = 1;
constexpr UINT kStatusTimerMs = 1000;

enum : UINT {
    IDM_CONNECT = 40001,
    IDM_DISCONNECT,
    IDM_SETTINGS,
    IDM_RUNASADMIN,
    IDM_EXIT,

    // One command per configured iPad, IDM_DEVICE_FIRST + index. Toggles that
    // device's sender on its own, so the others keep streaming.
    IDM_DEVICE_FIRST = 41000,
};

const wchar_t* const kWndClass = L"opendisplay-win-tray";

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

    // Names the iPads advertise over Bonjour, keyed by address — filled by a
    // background browse, because a lookup takes a moment and the menu must not
    // wait for it.
    mutable std::mutex discoveredMutex;
    std::map<std::string, std::string> discovered;
    std::thread discovery;
    std::atomic<bool> discoveryRunning{false};
};

// Rebuilds the sender list to match cfg.devices, stopping every running sender
// first. Called after the device list changed — a live stream survives only the
// settings changes that leave the list alone (see IDM_SETTINGS).
void RebuildSenders(TrayContext* ctx)
{
    for (auto& app : ctx->apps)
        app->Stop();
    ctx->apps.clear();
    for (size_t i = 0; i < ctx->cfg.devices.size(); ++i)
        ctx->apps.push_back(std::make_unique<SenderApp>());
}

// The label an iPad publishes for itself. Its Bonjour instance name is what the
// app's settings call the name — but that field falls back to the system name,
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

// Refreshes the advertised names in the background. Browsing takes a moment and
// names change rarely, so once a minute is plenty; the first pass runs
// immediately so the menu has names shortly after launch.
void RunDiscovery(TrayContext* ctx)
{
    constexpr int kBrowseMs = 800;
    constexpr int kPauseMs = 60'000;

    while (ctx->discoveryRunning) {
        std::map<std::string, std::string> names;
        for (const MdnsReceiver& receiver : BrowseReceivers(kBrowseMs))
            names[receiver.address] = ReceiverLabel(receiver);

        if (!names.empty()) {
            std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
            // Merged, not replaced: an iPad that is asleep doesn't answer, and
            // dropping its name would make the menu flip back to a raw address.
            for (auto& name : names)
                ctx->discovered[name.first] = name.second;
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

// Builds the tray icon at runtime (no .ico asset to ship): a little monitor
// glyph with a coloured status dot in the top-left corner. `dotColor` is the
// point — it signals connection state at a glance.
HICON MakeStatusIcon(COLORREF dotColor)
{
    constexpr int S = 32;
    HDC screen = GetDC(nullptr);
    HDC colorDC = CreateCompatibleDC(screen);
    HDC maskDC = CreateCompatibleDC(screen);
    HBITMAP colorBmp = CreateCompatibleBitmap(screen, S, S);
    HBITMAP maskBmp = CreateBitmap(S, S, 1, 1, nullptr); // monochrome AND mask
    ReleaseDC(nullptr, screen);

    HBITMAP oldColor = static_cast<HBITMAP>(SelectObject(colorDC, colorBmp));
    HBITMAP oldMask = static_cast<HBITMAP>(SelectObject(maskDC, maskBmp));

    RECT rc{0, 0, S, S};
    FillRect(colorDC, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));   // colour: black where transparent
    FillRect(maskDC, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));    // mask: 1 = transparent everywhere

    // Draws a shape on both DCs: real colour on colorDC, opaque (black=0) on
    // the mask so that region shows through.
    auto opaqueMask = [&] {
        SelectObject(maskDC, GetStockObject(BLACK_BRUSH));
        SelectObject(maskDC, GetStockObject(BLACK_PEN));
    };

    // Monitor body + stand.
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

    // Inner "screen" highlight.
    HBRUSH innerBrush = CreateSolidBrush(RGB(120, 150, 195));
    SelectObject(colorDC, innerBrush);
    SelectObject(colorDC, static_cast<HPEN>(GetStockObject(NULL_PEN)));
    RoundRect(colorDC, 5, 8, 27, 20, 3, 3);

    // Status dot, top-left corner, with a light outline for contrast.
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

// A configured device is "<address>[ <name>]": the iPad only ever calls itself
// "iPad" on the wire (iOS hands out the model, not the device name) and neither
// DNS nor mDNS resolves these addresses, so a readable label can only come from
// the user. Everything up to the first space is the address, the rest is the
// label; without one the address is the label.
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

std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
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

// Autostart via the per-user Run key (no admin needed, and the app runs
// un-elevated). Not a scheduled task — the app doesn't require elevation.
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
            std::wstring val = L"\"" + std::wstring(exe) + L"\""; // no args -> tray mode
            RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()),
                           static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// One line per iPad: "iPad-Pro 2732x2048" / "connecting..." / "off".
// Also the menu entry's text, so both say the same thing.
// What to call this iPad: the name the user typed in Settings wins, then the
// one it advertises over Bonjour, and the address is the fallback.
std::wstring DeviceLabel(const TrayContext* ctx, size_t index)
{
    DeviceEntry entry = SplitDevice(ctx->cfg.devices[index]);
    if (entry.label != entry.address)
        return Widen(entry.label);

    std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
    auto discovered = ctx->discovered.find(entry.address);
    return Widen(discovered != ctx->discovered.end() ? discovered->second : entry.address);
}

std::wstring DeviceStatusText(const TrayContext* ctx, size_t index)
{
    std::wstring text = DeviceLabel(ctx, index);
    const SenderApp& app = *ctx->apps[index];
    switch (app.GetState()) {
        case SenderApp::State::Streaming:
            return text + L"  " + std::to_wstring(app.Width()) + L"x" + std::to_wstring(app.Height());
        case SenderApp::State::Connecting:
            return text + L"  connecting...";
        case SenderApp::State::Blocked:
            // Only one panel size can be on the air (see AcquirePanel): name
            // the size that holds it, otherwise "waiting" looks like a hang.
            return text + L"  waiting for " + std::to_wstring(app.BlockedByWidth()) + L"x" +
                   std::to_wstring(app.BlockedByHeight());
        default:
            return text + L"  off";
    }
}

void UpdateStatus(TrayContext* ctx)
{
    std::wstring tip = L"opendisplay-win";
    if (ctx->cfg.devices.empty()) {
        tip += L" - no iPad configured";
    } else {
        // The tooltip is capped at 128 characters, so with several iPads the
        // tail may be cut — the menu carries the full list.
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
        ctx->apps[i]->Start(SplitDevice(ctx->cfg.devices[i]).address, ctx->cfg.port);
    UpdateStatus(ctx);
}

INT_PTR CALLBACK SettingsDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_INITDIALOG: {
            auto* cfg = reinterpret_cast<Config*>(lParam);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, static_cast<LONG_PTR>(lParam));
            std::wstring list;
            for (const std::string& device : cfg->devices)
                list += Widen(device) + L"\r\n"; // the edit control's line break
            SetDlgItemTextW(dlg, IDC_IP, list.c_str());
            SetDlgItemInt(dlg, IDC_PORT, cfg->port, FALSE);
            CheckDlgButton(dlg, IDC_AUTORECONNECT, cfg->autoReconnect ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_AUTOSTART, IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                auto* cfg = reinterpret_cast<Config*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));

                // One device per line, "<address> <name>" (see SplitDevice).
                // Blank lines and stray whitespace are dropped, and a repeated
                // address would only produce a second sender that the per-iPad
                // lock refuses — so drop those too, name or no name.
                wchar_t buf[1024] = {};
                GetDlgItemTextW(dlg, IDC_IP, buf, static_cast<int>(std::size(buf)));
                cfg->devices.clear();
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
                        std::string address = SplitDevice(device).address;
                        bool known = false;
                        for (const std::string& existing : cfg->devices)
                            known = known || SplitDevice(existing).address == address;
                        if (!known)
                            cfg->devices.push_back(device);
                    }
                    pos = end + 1;
                }
                BOOL ok = FALSE;
                UINT port = GetDlgItemInt(dlg, IDC_PORT, &ok, FALSE);
                if (ok && port > 0 && port <= 65535)
                    cfg->port = static_cast<uint16_t>(port);
                cfg->autoReconnect = IsDlgButtonChecked(dlg, IDC_AUTORECONNECT) == BST_CHECKED;
                SetAutostart(IsDlgButtonChecked(dlg, IDC_AUTOSTART) == BST_CHECKED);
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

void ShowContextMenu(HWND hwnd, TrayContext* ctx)
{
    HMENU menu = CreatePopupMenu();

    // One checkable entry per iPad: click toggles just that one, so the others
    // keep streaming.
    bool several = ctx->cfg.devices.size() > 1;
    for (size_t i = 0; i < ctx->cfg.devices.size(); ++i) {
        // Checked follows the *state*, not IsRunning(): a sender that backed off
        // because another instance already holds this iPad keeps its thread
        // object but sits Idle, and showing that as connected would lie.
        UINT flags = MF_STRING | (ctx->apps[i]->GetState() != SenderApp::State::Idle ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, IDM_DEVICE_FIRST + i, DeviceStatusText(ctx, i).c_str());
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
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings...");
    if (!IsElevated())
        AppendMenuW(menu, MF_STRING, IDM_RUNASADMIN, L"Run as administrator");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // so the menu dismisses when clicking elsewhere
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<TrayContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_APP_TRAY:
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
                ShowContextMenu(hwnd, ctx);
            else if (LOWORD(lParam) == WM_LBUTTONDBLCLK)
                DialogBoxParamW(ctx->hInstance, MAKEINTRESOURCEW(IDD_SETTINGS), hwnd, SettingsDlgProc,
                                reinterpret_cast<LPARAM>(&ctx->cfg));
            return 0;

        case WM_COMMAND:
            // One iPad's entry: toggle that sender alone.
            if (LOWORD(wParam) >= IDM_DEVICE_FIRST && LOWORD(wParam) < IDM_DEVICE_FIRST + ctx->apps.size()) {
                size_t index = LOWORD(wParam) - IDM_DEVICE_FIRST;
                SenderApp& app = *ctx->apps[index];
                if (app.IsRunning())
                    app.Stop();
                else
                    app.Start(SplitDevice(ctx->cfg.devices[index]).address, ctx->cfg.port);
                UpdateStatus(ctx);
                return 0;
            }

            switch (LOWORD(wParam)) {
                case IDM_CONNECT:
                    for (size_t i = 0; i < ctx->apps.size(); ++i)
                        ctx->apps[i]->Start(SplitDevice(ctx->cfg.devices[i]).address, ctx->cfg.port);
                    UpdateStatus(ctx);
                    return 0;
                case IDM_DISCONNECT:
                    for (auto& app : ctx->apps)
                        app->Stop();
                    UpdateStatus(ctx);
                    return 0;
                case IDM_SETTINGS: {
                    std::vector<std::string> oldDevices = ctx->cfg.devices;
                    uint16_t oldPort = ctx->cfg.port;
                    if (DialogBoxParamW(ctx->hInstance, MAKEINTRESOURCEW(IDD_SETTINGS), hwnd, SettingsDlgProc,
                                        reinterpret_cast<LPARAM>(&ctx->cfg)) == IDOK) {
                        ctx->cfg.Save();
                        // Only tear down and reconnect if the *targets* actually
                        // changed — toggling auto-connect/autostart, or OK with
                        // no change, must not drop a live stream.
                        if (ctx->cfg.devices != oldDevices || ctx->cfg.port != oldPort)
                            ApplyAndRestart(ctx);
                    }
                    return 0;
                }
                case IDM_RUNASADMIN: {
                    // Relaunch elevated, then exit this instance. Stop first so
                    // the per-iPad locks are released before the elevated copy
                    // grabs them. Elevated lets touch reach elevated windows and
                    // register new resolutions without a separate prompt.
                    for (auto& app : ctx->apps)
                        app->Stop();
                    wchar_t exe[MAX_PATH];
                    GetModuleFileNameW(nullptr, exe, MAX_PATH);
                    SHELLEXECUTEINFOW sei{};
                    sei.cbSize = sizeof(sei);
                    sei.lpVerb = L"runas";
                    sei.lpFile = exe;
                    sei.nShow = SW_NORMAL;
                    if (ShellExecuteExW(&sei))
                        DestroyWindow(hwnd); // elevated copy is up; tear this one down
                    else
                        ApplyAndRestart(ctx); // UAC declined: resume as we were
                    return 0;
                }
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
            }
            return 0;

        case WM_TIMER:
            if (wParam == kStatusTimerId)
                UpdateStatus(ctx);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kStatusTimerId);
            Shell_NotifyIconW(NIM_DELETE, &ctx->nid);
            for (auto& app : ctx->apps)
                app->Stop();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int RunTray(HINSTANCE hInstance)
{
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

    HWND hwnd = CreateWindowExW(0, kWndClass, L"opendisplay-win", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance,
                                nullptr);
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
    wcsncpy_s(ctx.nid.szTip, L"opendisplay-win", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &ctx.nid);

    SetTimer(hwnd, kStatusTimerId, kStatusTimerMs, nullptr);

    ctx.discoveryRunning = true;
    ctx.discovery = std::thread([&ctx] { RunDiscovery(&ctx); });

    // Auto-connect on launch: every configured iPad, each on its own sender.
    if (ctx.cfg.autoReconnect)
        for (size_t i = 0; i < ctx.apps.size(); ++i)
            ctx.apps[i]->Start(SplitDevice(ctx.cfg.devices[i]).address, ctx.cfg.port);
    UpdateStatus(&ctx);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ctx.discoveryRunning = false;
    if (ctx.discovery.joinable())
        ctx.discovery.join();

    DestroyIcon(ctx.iconGreen);
    DestroyIcon(ctx.iconRed);
    DestroyIcon(ctx.iconGrey);
    return 0;
}

} // namespace od
