#include "app/TrayApp.h"

#include "app/Config.h"
#include "app/SenderApp.h"
#include "app/resources.h"

#include <shellapi.h>

#include <string>

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
};

const wchar_t* const kWndClass = L"opendisplay-win-tray";

struct TrayContext {
    HINSTANCE hInstance = nullptr;
    SenderApp app;
    Config cfg;
    NOTIFYICONDATAW nid{};
    HICON iconGreen = nullptr; // streaming
    HICON iconRed = nullptr;   // configured but not connected
    HICON iconGrey = nullptr;  // no iPad configured
};

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
    if (ctx->cfg.ip.empty())
        return ctx->iconGrey;
    if (ctx->app.GetState() == SenderApp::State::Streaming)
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

void UpdateStatus(TrayContext* ctx)
{
    std::wstring ip = Widen(ctx->cfg.ip);
    std::wstring tip;
    switch (ctx->app.GetState()) {
        case SenderApp::State::Streaming:
            tip = L"opendisplay-win - " + ip + L" (" + std::to_wstring(ctx->app.Width()) + L"x" +
                  std::to_wstring(ctx->app.Height()) + L")";
            break;
        case SenderApp::State::Connecting:
            tip = L"opendisplay-win - connecting to " + (ip.empty() ? L"?" : ip) + L"...";
            break;
        default:
            tip = ip.empty() ? L"opendisplay-win - no iPad configured" : L"opendisplay-win - not connected";
            break;
    }

    ctx->nid.hIcon = PickIcon(ctx);
    wcsncpy_s(ctx->nid.szTip, tip.c_str(), _TRUNCATE);
    ctx->nid.uFlags = NIF_ICON | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &ctx->nid);
}

void ApplyAndRestart(TrayContext* ctx)
{
    ctx->app.Stop();
    if (!ctx->cfg.ip.empty())
        ctx->app.Start(ctx->cfg.ip, ctx->cfg.port);
    UpdateStatus(ctx);
}

INT_PTR CALLBACK SettingsDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_INITDIALOG: {
            auto* cfg = reinterpret_cast<Config*>(lParam);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, static_cast<LONG_PTR>(lParam));
            SetDlgItemTextW(dlg, IDC_IP, Widen(cfg->ip).c_str());
            SetDlgItemInt(dlg, IDC_PORT, cfg->port, FALSE);
            CheckDlgButton(dlg, IDC_AUTORECONNECT, cfg->autoReconnect ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_AUTOSTART, IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                auto* cfg = reinterpret_cast<Config*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
                wchar_t buf[64] = {};
                GetDlgItemTextW(dlg, IDC_IP, buf, static_cast<int>(std::size(buf)));
                cfg->ip = Narrow(buf);
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
    if (ctx->app.IsRunning()) {
        AppendMenuW(menu, MF_STRING, IDM_DISCONNECT, L"Disconnect");
    } else {
        AppendMenuW(menu, MF_STRING | (ctx->cfg.ip.empty() ? MF_GRAYED : 0), IDM_CONNECT, L"Connect");
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
            switch (LOWORD(wParam)) {
                case IDM_CONNECT:
                    if (!ctx->cfg.ip.empty())
                        ctx->app.Start(ctx->cfg.ip, ctx->cfg.port);
                    UpdateStatus(ctx);
                    return 0;
                case IDM_DISCONNECT:
                    ctx->app.Stop();
                    UpdateStatus(ctx);
                    return 0;
                case IDM_SETTINGS: {
                    std::string oldIp = ctx->cfg.ip;
                    uint16_t oldPort = ctx->cfg.port;
                    if (DialogBoxParamW(ctx->hInstance, MAKEINTRESOURCEW(IDD_SETTINGS), hwnd, SettingsDlgProc,
                                        reinterpret_cast<LPARAM>(&ctx->cfg)) == IDOK) {
                        ctx->cfg.Save();
                        // Only tear down and reconnect if the *target* actually
                        // changed — toggling auto-connect/autostart, or OK with
                        // no change, must not drop a live stream.
                        if (ctx->cfg.ip != oldIp || ctx->cfg.port != oldPort)
                            ApplyAndRestart(ctx);
                    }
                    return 0;
                }
                case IDM_RUNASADMIN: {
                    // Relaunch elevated, then exit this instance. Stop first so
                    // the per-iPad lock is released before the elevated copy
                    // grabs it. Elevated lets touch reach elevated windows and
                    // register new resolutions without a separate prompt.
                    ctx->app.Stop();
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
            ctx->app.Stop();
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

    // Auto-connect on launch if configured and an iPad is set.
    if (ctx.cfg.autoReconnect && !ctx.cfg.ip.empty())
        ctx.app.Start(ctx.cfg.ip, ctx.cfg.port);
    UpdateStatus(&ctx);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyIcon(ctx.iconGreen);
    DestroyIcon(ctx.iconRed);
    DestroyIcon(ctx.iconGrey);
    return 0;
}

} // namespace od
