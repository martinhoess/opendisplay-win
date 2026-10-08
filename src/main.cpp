#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <string>

#include <winsock2.h>

#include <mfapi.h>

#pragma comment(lib, "mfplat.lib")

#include "app/SenderApp.h"
#include "app/TrayApp.h"
#include "display/MttVddSettings.h"
#include "net/Mdns.h"


namespace {

// Route diagnostics to %APPDATA%\opendisplay-win\log.txt. Under the GUI
// subsystem there is no console to print to, and a file gives persistent logs
// regardless of how the app was launched.
// `name` is the log's file name: the tray owns log.txt, while a headless CLI
// sender gets its own log-<pid>.txt. Both open CREATE_ALWAYS, so without the
// split a second process would truncate the first one's log out from under it.
void RedirectLogToFile(const std::string& name)
{
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") != 0 || appdata == nullptr)
        return;
    std::string dir = std::string(appdata) + "\\opendisplay-win";
    free(appdata);

    CreateDirectoryA(dir.c_str(), nullptr); // no-op if it already exists
    std::string logPath = dir + "\\" + name;

    // Under the GUI subsystem there is no console, so stdout/stderr have no
    // valid fd to redirect. Bind them to NUL first to give them real fds, then
    // dup our log handle over those. (freopen straight to the log would work
    // too but opens exclusive — this keeps the log tailable while running.)
    FILE* f = nullptr;
    freopen_s(&f, "NUL", "w", stdout);
    freopen_s(&f, "NUL", "w", stderr);

    // Keep the previous run's log (e.g. what Quit / session end did) as
    // log.prev.txt instead of truncating it away.
    if (name == "log.txt")
        (void)MoveFileExA(logPath.c_str(), (dir + "\\log.prev.txt").c_str(), MOVEFILE_REPLACE_EXISTING);

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

// The one-off commands below are meant to be read in the terminal that started
// them, but this is a GUI-subsystem process and has no console of its own — so
// borrow the caller's. Returns false when there is none (double-clicked, or
// started by a process without a console), and the log file takes over.
bool AttachParentConsole()
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return false;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

// Last-resort diagnostics: on an unhandled crash, write the exception code and
// the faulting module + offset to the log before dying. Without this the tray
// process just vanishes with no WER entry, so a crash during a resolution/
// rotation change left nothing to point at the culprit.
LONG WINAPI CrashLogger(EXCEPTION_POINTERS* ep)
{
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    HMODULE mod = nullptr;
    char modName[MAX_PATH] = "?";
    uintptr_t off = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod)) {
        GetModuleFileNameA(mod, modName, MAX_PATH);
        off = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
    }
    fprintf(stderr, "\n*** CRASH: code=0x%08lX addr=%p module=%s +0x%zX ***\n", code, addr, modName, off);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER; // let the process terminate
}

// "WxH" in argv[i], or "W H" in argv[i], argv[i+1]. False if absent/invalid.
bool ParseHelloSizeArgs(int argc, char** argv, int i, uint32_t& w, uint32_t& h)
{
    if (i >= argc)
        return false;
    unsigned long a = 0, b = 0;
    char* end = nullptr;
    a = strtoul(argv[i], &end, 10);
    if (end && (*end == 'x' || *end == 'X')) {
        b = strtoul(end + 1, nullptr, 10);
    } else if (i + 1 < argc) {
        b = strtoul(argv[i + 1], nullptr, 10);
    }
    if (a == 0 || b == 0 || a > 16384 || b > 16384)
        return false;
    w = static_cast<uint32_t>(a);
    h = static_cast<uint32_t>(b);
    return true;
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

    std::string command = argc >= 2 ? argv[1] : "";
    bool oneOff = command == "--browse-mdns" ||
                  command == "--ensure-mtt-resolutions" || command == "--ensure-mtt-edid" ||
                  command == "--dump-mtt-edid" ||
                  command == "--set-mtt-monitor-count" ||
                  command == "--teardown-mtt-vdd" || command == "--ensure-mtt-vdd";

    // A one-off answers into the caller's terminal; a sender writes to the log,
    // and a headless one gets its own file so two of them don't truncate each
    // other's.
    if (!oneOff || !AttachParentConsole())
        RedirectLogToFile(argc >= 2 ? "log-" + std::to_string(GetCurrentProcessId()) + ".txt" : "log.txt");

    SetUnhandledExceptionFilter(CrashLogger);

    // Clear leftover DirectX UserGpuPreferences for this exe (Intel-first DXGI).
    if (!oneOff) {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) && exePath[0]) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
                              0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
                RegDeleteValueW(key, exePath);
                RegCloseKey(key);
            }
        }
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    int rc = 0;
    if ((argc >= 2 && std::string(argv[1]) == "--ensure-mtt-edid") ||
               (argc >= 2 && std::string(argv[1]) == "--ensure-mtt-resolutions")) {
        // Elevated one-off (SelfElevateEnsure): with a hello size WxH, generate
        // that iPad's EDID + XML modes; without, install the shipped
        // user_edid.bin. Sets CustomEdid/PreventSpoof, reloads MttVDD only if
        // something changed.
        uint32_t helloW = 0, helloH = 0;
        od::MttEnsureResult r = ParseHelloSizeArgs(argc, argv, 2, helloW, helloH)
                                    ? od::EnsureCustomEdidForHello(helloW, helloH, /*dryRun=*/false)
                                    : od::EnsureCustomEdid();
        const bool ok = r.ok;
        printf("ensure-mtt-edid: %s (added %d, changed=%d) path=%ls detail=%s\n",
               ok ? "ok" : "failed", r.added, r.changed ? 1 : 0, r.path.c_str(), r.detail.c_str());
        rc = ok ? 0 : 1;

    } else if (argc >= 2 && std::string(argv[1]) == "--dump-mtt-edid") {
        // Diagnostics (no admin, touches nothing): --dump-mtt-edid WxH [out.bin]
        // prints the generated mode list and writes the EDID bytes, for
        // byte-comparison with tools/gen_user_edid.py --size WxH.
        uint32_t helloW = 0, helloH = 0;
        if (!ParseHelloSizeArgs(argc, argv, 2, helloW, helloH)) {
            printf("usage: --dump-mtt-edid WxH [out.bin]\n");
            rc = 1;
        } else {
            const std::vector<od::MttMode> modes = od::MttModesForHello(helloW, helloH);
            const std::vector<uint8_t> edid = od::BuildMttEdid(modes);
            printf("%ux%u:", helloW, helloH);
            for (size_t i = 0; i < modes.size(); ++i)
                printf("%s %ux%u@%u", i ? "," : "", modes[i].width, modes[i].height, modes[i].hz);
            printf("\n");
            const bool sizeIsOneArg = strchr(argv[2], 'x') != nullptr || strchr(argv[2], 'X') != nullptr;
            const int outIdx = sizeIsOneArg ? 3 : 4;
            if (edid.size() != 128) {
                printf("EDID generation failed\n");
                rc = 1;
            } else if (outIdx < argc) {
                FILE* f = nullptr;
                if (fopen_s(&f, argv[outIdx], "wb") == 0 && f) {
                    fwrite(edid.data(), 1, edid.size(), f);
                    fclose(f);
                    printf("wrote %s (128 bytes)\n", argv[outIdx]);
                } else {
                    printf("cannot write %s\n", argv[outIdx]);
                    rc = 1;
                }
            }
        }
    } else if (argc >= 3 && std::string(argv[1]) == "--set-mtt-monitor-count") {
        // Elevated one-shot: set <monitors><count> in live MTT settings and reload.
        // Invoked by SelfElevateSetMonitorCount (ensure/teardown path).
        auto n = static_cast<uint32_t>(strtoul(argv[2], nullptr, 10));
        od::MttEnsureResult r = od::SetMonitorCount(n);
        printf("set-mtt-monitor-count %u: %s changed=%d detail=%s\n", n, r.ok ? "ok" : "failed",
               r.changed ? 1 : 0, r.detail.c_str());
        rc = r.ok ? 0 : 1;
    } else if (argc >= 2 && std::string(argv[1]) == "--teardown-mtt-vdd") {
        // Verify last-client teardown path without an iPad session.
        bool ok = od::TearDownMttVdd("cli");
        printf("teardown-mtt-vdd: %s\n", ok ? "ok" : "failed");
        rc = ok ? 0 : 1;
    } else if (argc >= 2 && std::string(argv[1]) == "--ensure-mtt-vdd") {
        bool ok = od::EnsureMttVddAttached("cli");
        printf("ensure-mtt-vdd: %s\n", ok ? "ok" : "failed");
        rc = ok ? 0 : 1;
    } else if (argc >= 2 && std::string(argv[1]) == "--browse-mdns") {
        // Checks the response parser against malformed packets, then shows what
        // is actually advertising right now — the parser reads data from
        // whoever answers on the network, so it gets a probe of its own.
        bool ok = od::SelfCheck();
        for (const od::MdnsReceiver& receiver : od::BrowseReceivers(1500))
            printf("%s  %s  host=%s port=%u id=%s\n", receiver.address.c_str(), receiver.instance.c_str(),
                   receiver.host.c_str(), receiver.port, receiver.id.c_str());
        rc = ok ? 0 : 1;
    } else if (argc >= 2 && (std::string(argv[1]) == "--register-resolution" ||
                              std::string(argv[1]) == "--remove-display" ||
                              std::string(argv[1]) == "--cleanup-monitors")) {
        printf("%s: removed (MTT-only fork; use --ensure-mtt-* / --teardown-mtt-vdd)\n", argv[1]);
        rc = 1;
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
