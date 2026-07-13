#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <string>

#include <winsock2.h>

#include <mfapi.h>

#pragma comment(lib, "mfplat.lib")

#include "app/SenderApp.h"
#include "app/TrayApp.h"
#include "display/VirtualDisplay.h"

namespace {

// Route diagnostics to %APPDATA%\opendisplay-win\log.txt. Under the GUI
// subsystem there is no console to print to, and a file gives persistent logs
// regardless of how the app was launched.
void RedirectLogToFile()
{
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") != 0 || appdata == nullptr)
        return;
    std::string dir = std::string(appdata) + "\\opendisplay-win";
    free(appdata);

    CreateDirectoryA(dir.c_str(), nullptr); // no-op if it already exists
    std::string logPath = dir + "\\log.txt";

    // Under the GUI subsystem there is no console, so stdout/stderr have no
    // valid fd to redirect. Bind them to NUL first to give them real fds, then
    // dup our log handle over those. (freopen straight to the log would work
    // too but opens exclusive — this keeps the log tailable while running.)
    FILE* f = nullptr;
    freopen_s(&f, "NUL", "w", stdout);
    freopen_s(&f, "NUL", "w", stderr);

    HANDLE h = CreateFileA(logPath.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_TEXT);
    if (fd == -1) {
        CloseHandle(h);
        return;
    }
    _dup2(fd, _fileno(stdout));
    _dup2(fd, _fileno(stderr));
    _close(fd); // the dup'd copies on stdout/stderr keep the file open
    setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered so a live stream shows immediately
    setvbuf(stderr, nullptr, _IONBF, 0);
}

} // namespace

int main(int argc, char** argv)
{
    // Run per-monitor DPI aware (V2): on a scaled desktop (e.g. 125%) a
    // DPI-unaware process sees monitor positions in *virtualized* coordinates
    // from GetMonitorInfo but sets them in *physical* coordinates via
    // ChangeDisplaySettingsEx, so a saved position wouldn't round-trip. Being
    // DPI-aware keeps every coordinate API in one physical space — needed for
    // stable position persistence and correct input mapping on scaled displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Keep COM (MTA) and Media Foundation alive for the whole process. The
    // per-connection H264Encoder does its own CoInitialize/MFStartup on its
    // worker thread and the matching CoUninitialize/MFShutdown on Stop(); with
    // this process-wide reference those only *decrement* the refcounts instead
    // of tearing the subsystems down while DXGI/MF background threads are still
    // attached — which crashed the app on Disconnect. Intentionally never
    // released (process-lifetime).
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    RedirectLogToFile();

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    int rc = 0;
    if (argc >= 4 && std::string(argv[1]) == "--register-resolution") {
        // Elevated one-off: register a custom resolution (+ its rotation) so the
        // virtual monitor can use it. Invoked by the self-elevate path or by hand.
        auto w = static_cast<uint32_t>(strtoul(argv[2], nullptr, 10));
        auto h = static_cast<uint32_t>(strtoul(argv[3], nullptr, 10));
        bool ok = w > 0 && h > 0 && od::VirtualDisplay::RegisterResolutions(w, h);
        printf("register %ux%u: %s\n", w, h, ok ? "ok" : "failed");
        rc = ok ? 0 : 1;
    } else if (argc >= 2 && std::string(argv[1]) == "--cleanup-monitors") {
        // Explicit one-off: drop phantom virtual monitors from earlier runs.
        int n = od::VirtualDisplay::CleanupGhostMonitors();
        printf("removed %d leftover virtual monitor(s)\n", n);
    } else if (argc >= 2) {
        // Headless CLI mode: stream to the given IP until killed (handy for
        // testing/scripting; logging goes to the inherited/redirected stdout).
        // The per-iPad connection guard lives in SenderApp.
        od::SenderApp app;
        app.RunBlocking(argv[1], 9000);
    } else {
        // No IP argument: run the tray GUI (settings, connect/disconnect).
        rc = od::RunTray(GetModuleHandleW(nullptr));
    }

    WSACleanup();
    return rc;
}
