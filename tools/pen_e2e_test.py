#!/usr/bin/env python3
"""End-to-end check: mock receiver -> opendisplay-win -> Windows pen input.

Moves the window running tools/pen_test.html onto the virtual monitor, waits for
the stroke played by mock_receiver.py --pencil-stroke, and reads pointer type,
pressure and tilt back out of the window title. The original window position is
restored even if the measurement fails.

Injected pen input does not move the mouse cursor, so watching GetCursorPos is
not a valid test — a pointer-aware window is.

Usage:
  python3 tools/pen_e2e_test.py [--width 2732] [--height 2048] [--seconds 25]
"""
import argparse
import ctypes
import re
import sys
import time
from ctypes import wintypes

u = ctypes.windll.user32
u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))

MARKER = "Pen-Test"
SWP_NOZORDER = 0x0004
SW_RESTORE = 9


def title(hwnd):
    n = u.GetWindowTextLengthW(hwnd)
    b = ctypes.create_unicode_buffer(n + 1)
    u.GetWindowTextW(hwnd, b, n + 1)
    return b.value


def find_windows():
    """Every visible window carrying the test page's title marker."""
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        if u.IsWindowVisible(hwnd) and MARKER in title(hwnd):
            found.append(hwnd)
        return True

    u.EnumWindows(cb, 0)
    return found


def virtual_monitor(width, height):
    """Rect of the monitor matching the iPad panel size."""
    hits = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HANDLE, wintypes.HDC,
                        ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)
    def cb(hmon, hdc, lprect, lparam):
        r = lprect.contents
        if (r.right - r.left, r.bottom - r.top) == (width, height):
            hits.append((r.left, r.top, r.right, r.bottom))
        return True

    u.EnumDisplayMonitors(None, None, cb, 0)
    return hits[0] if hits else None


def measure(hwnd, seconds):
    """Poll the title and return (saw_pen, highest pressure, last title)."""
    saw_pen = False
    best = 0.0
    deadline = time.time() + seconds
    while time.time() < deadline:
        t = title(hwnd)
        if f"{MARKER} pen " in t:
            saw_pen = True
            m = re.search(r"p=([0-9.]+)", t)
            if m:
                best = max(best, float(m.group(1)))
        time.sleep(0.05)
    return saw_pen, best, title(hwnd)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=2732, help="iPad panel width, identifies the monitor")
    ap.add_argument("--height", type=int, default=2048)
    ap.add_argument("--seconds", type=float, default=25.0, help="how long to watch for pen events")
    args = ap.parse_args()

    windows = find_windows()
    if not windows:
        print(f"No window with {MARKER!r} in its title - is tools/pen_test.html open?")
        return 1
    if len(windows) > 1:
        # The window gets moved onto a monitor the user cannot see, so never
        # guess which one is meant.
        print(f"{len(windows)} windows carry the marker - close all but one:")
        for h in windows:
            print(f"  {title(h)!r}")
        return 1
    hwnd = windows[0]
    print(f"window: {title(hwnd)!r}")

    mon = virtual_monitor(args.width, args.height)
    if not mon:
        print(f"No {args.width}x{args.height} monitor found - is the sender running?")
        return 1
    print(f"virtual monitor: {mon}")

    old = wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(old))
    left, top, right, bottom = mon

    try:
        u.ShowWindow(hwnd, SW_RESTORE)
        u.SetWindowPos(hwnd, None, left, top, right - left, bottom - top, SWP_NOZORDER)
        u.SetForegroundWindow(hwnd)
        time.sleep(1.0)
        print("waiting for the mock receiver's stroke ...")
        saw_pen, best, last = measure(hwnd, args.seconds)
    finally:
        # Always put the window back: on the virtual monitor it is invisible,
        # and it disappears entirely once the sender drops that monitor.
        u.SetWindowPos(hwnd, None, old.left, old.top, old.right - old.left,
                       old.bottom - old.top, SWP_NOZORDER)

    print(f"last title: {last!r}")
    print(f"pen events seen: {'yes' if saw_pen else 'no'}")
    print(f"highest pressure: {best:.2f}")
    return 0 if saw_pen else 1


if __name__ == "__main__":
    sys.exit(main())
