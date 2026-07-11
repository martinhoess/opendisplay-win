#include <cstdio>

#include <winsock2.h>

#include "app/SenderApp.h"

int main(int argc, char** argv)
{
    // Run per-monitor DPI aware (V2): on a scaled desktop (e.g. 125%) a
    // DPI-unaware process sees monitor positions in *virtualized* coordinates
    // from GetMonitorInfo but sets them in *physical* coordinates via
    // ChangeDisplaySettingsEx, so a saved position wouldn't round-trip. Being
    // DPI-aware keeps every coordinate API in one physical space — needed for
    // stable position persistence and correct input mapping on scaled displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // stdout is fully buffered (not line-buffered) once redirected to a file
    // or pipe, so status printf()s otherwise only show up after ~4KB or on
    // exit — useless for watching a long-running stream live.
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <ipad-ip>\n", argv[0]);
        return 1;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    od::SenderApp app(argv[1]);
    app.Run(); // blocks forever, reconnecting on drop

    WSACleanup();
    return 0;
}
