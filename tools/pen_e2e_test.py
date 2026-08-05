"""End-to-End-Nachweis: Mock-Empfaenger -> opendisplay-win -> Windows-Stifteingabe.

Schiebt das Fenster mit pen_test.html auf den virtuellen Monitor, wartet auf den
Strich des Mock-Empfaengers und liest Typ und Druck aus dem Fenstertitel.
Die urspruengliche Fensterposition wird am Ende wiederhergestellt.
"""
import ctypes
import sys
import time
from ctypes import wintypes

u = ctypes.windll.user32
u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))

MARKER = "Pen-Test"
PANEL_W, PANEL_H = 2732, 2048


def find_window():
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        if not u.IsWindowVisible(hwnd):
            return True
        n = u.GetWindowTextLengthW(hwnd)
        if n == 0:
            return True
        b = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(hwnd, b, n + 1)
        if MARKER in b.value or b.value.startswith(("pen ", "mouse ")):
            found.append(hwnd)
        return True

    u.EnumWindows(cb, 0)
    return found[0] if found else None


def title(hwnd):
    n = u.GetWindowTextLengthW(hwnd)
    b = ctypes.create_unicode_buffer(n + 1)
    u.GetWindowTextW(hwnd, b, n + 1)
    return b.value


def virtual_monitor():
    """Rechteck des Monitors mit der iPad-Panelgroesse."""
    hits = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HANDLE, wintypes.HDC,
                        ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)
    def cb(hmon, hdc, lprect, lparam):
        r = lprect.contents
        if (r.right - r.left, r.bottom - r.top) == (PANEL_W, PANEL_H):
            hits.append((r.left, r.top, r.right, r.bottom))
        return True

    u.EnumDisplayMonitors(None, None, cb, 0)
    return hits[0] if hits else None


def main():
    hwnd = find_window()
    if not hwnd:
        print("Testseite nicht gefunden.")
        return 1

    mon = virtual_monitor()
    if not mon:
        print(f"Kein Monitor mit {PANEL_W}x{PANEL_H} gefunden - laeuft der Sender?")
        return 1
    print(f"virtueller Monitor: {mon}")

    old = wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(old))

    left, top, right, bottom = mon
    u.ShowWindow(hwnd, 9)
    u.SetWindowPos(hwnd, None, left, top, right - left, bottom - top, 0x0004)
    u.SetForegroundWindow(hwnd)
    time.sleep(1.0)
    print(f"Fenster auf den virtuellen Monitor geschoben, Titel jetzt: {title(hwnd)!r}")

    print("warte auf den Strich des Mock-Empfaengers ...")
    best_pressure = 0.0
    saw_pen = False
    deadline = time.time() + 25
    while time.time() < deadline:
        t = title(hwnd)
        if t.startswith("pen "):
            saw_pen = True
            try:
                best_pressure = max(best_pressure, float(t.split("p=")[1].split(" ")[0]))
            except (IndexError, ValueError):
                pass
        time.sleep(0.05)

    u.SetWindowPos(hwnd, None, old.left, old.top, old.right - old.left,
                   old.bottom - old.top, 0x0004)

    print(f"letzter Titel: {title(hwnd)!r}")
    print(f"Stiftereignisse gesehen: {'JA' if saw_pen else 'NEIN'}")
    print(f"hoechster gemessener Druck: {best_pressure:.2f}")
    return 0 if saw_pen else 1


if __name__ == "__main__":
    sys.exit(main())
