"""Ändert den virtuellen parsec-vdd-Monitor Windows-seitig, ohne dass ein `hello`
vom iPad kommt — genau der Pfad, auf dem die Rotations-Regression saß.

    python tools/rotate_virtual_display.py             # zeigt aktuellen Zustand
    python tools/rotate_virtual_display.py modes       # listet die registrierten Modi
    python tools/rotate_virtual_display.py portrait    # nur Komposition drehen (DM_DISPLAYORIENTATION)
    python tools/rotate_virtual_display.py landscape   # zurück
    python tools/rotate_virtual_display.py 2048x2732   # echter Moduswechsel (Panel selbst wird hochkant)

Der Unterschied ist der Kern: `portrait` dreht nur die Ausgabe, die DXGI-Textur bleibt
quer, die Frame-Maße ändern sich also gar nicht. Erst der Moduswechsel trifft den Fall,
in dem der Encoder früher auf der alten Geometrie sitzen blieb. Zum Nachweis den Strom
mit `tools/mock_receiver.py --dump-h264 <datei>` mitschneiden und mit
`tools/sps_resolutions.py` auswerten.
"""
import ctypes
import sys
from ctypes import wintypes

user32 = ctypes.windll.user32

DMDO_DEFAULT, DMDO_90 = 0, 1
DM_DISPLAYORIENTATION, DM_PELSWIDTH, DM_PELSHEIGHT = 0x80, 0x00080000, 0x00100000
ENUM_CURRENT_SETTINGS = -1
CDS_UPDATEREGISTRY = 0x01


class DEVMODE(ctypes.Structure):
    _fields_ = [
        ("dmDeviceName", wintypes.WCHAR * 32),
        ("dmSpecVersion", wintypes.WORD),
        ("dmDriverVersion", wintypes.WORD),
        ("dmSize", wintypes.WORD),
        ("dmDriverExtra", wintypes.WORD),
        ("dmFields", wintypes.DWORD),
        ("dmPositionX", ctypes.c_long),
        ("dmPositionY", ctypes.c_long),
        ("dmDisplayOrientation", wintypes.DWORD),
        ("dmDisplayFixedOutput", wintypes.DWORD),
        ("dmColor", ctypes.c_short),
        ("dmDuplex", ctypes.c_short),
        ("dmYResolution", ctypes.c_short),
        ("dmTTOption", ctypes.c_short),
        ("dmCollate", ctypes.c_short),
        ("dmFormName", wintypes.WCHAR * 32),
        ("dmLogPixels", wintypes.WORD),
        ("dmBitsPerPel", wintypes.DWORD),
        ("dmPelsWidth", wintypes.DWORD),
        ("dmPelsHeight", wintypes.DWORD),
        ("dmDisplayFlags", wintypes.DWORD),
        ("dmDisplayFrequency", wintypes.DWORD),
        ("dmICMMethod", wintypes.DWORD),
        ("dmICMIntent", wintypes.DWORD),
        ("dmMediaType", wintypes.DWORD),
        ("dmDitherType", wintypes.DWORD),
        ("dmReserved1", wintypes.DWORD),
        ("dmReserved2", wintypes.DWORD),
        ("dmPanningWidth", wintypes.DWORD),
        ("dmPanningHeight", wintypes.DWORD),
    ]


class DISPLAY_DEVICE(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("DeviceName", wintypes.WCHAR * 32),
        ("DeviceString", wintypes.WCHAR * 128),
        ("StateFlags", wintypes.DWORD),
        ("DeviceID", wintypes.WCHAR * 128),
        ("DeviceKey", wintypes.WCHAR * 128),
    ]


def find_virtual_display():
    """GDI-Name des parsec-vdd-Monitors, z. B. \\\\.\\DISPLAY3."""
    dev = DISPLAY_DEVICE()
    dev.cb = ctypes.sizeof(dev)
    i = 0
    while user32.EnumDisplayDevicesW(None, i, ctypes.byref(dev), 0):
        if "parsec" in dev.DeviceString.lower() and dev.StateFlags & 0x01:  # ATTACHED_TO_DESKTOP
            return dev.DeviceName, dev.DeviceString
        i += 1
    raise SystemExit("kein angeschlossener parsec-vdd-Monitor gefunden")


def get_current_mode(name):
    dm = DEVMODE()
    dm.dmSize = ctypes.sizeof(dm)
    if not user32.EnumDisplaySettingsW(name, ENUM_CURRENT_SETTINGS, ctypes.byref(dm)):
        raise SystemExit(f"EnumDisplaySettings für {name} fehlgeschlagen")
    return dm


def list_modes(name):
    dm = DEVMODE()
    dm.dmSize = ctypes.sizeof(dm)
    i, gesehen = 0, []
    while user32.EnumDisplaySettingsW(name, i, ctypes.byref(dm)):
        eintrag = (dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency)
        if eintrag not in gesehen:
            gesehen.append(eintrag)
        i += 1
    return gesehen


def apply_mode(name, dm, was):
    ergebnis = user32.ChangeDisplaySettingsExW(name, ctypes.byref(dm), None, CDS_UPDATEREGISTRY, None)
    print(f"{was}: ChangeDisplaySettingsEx -> {ergebnis} (0 = OK, -2 = Modus nicht registriert)")
    return ergebnis


def rotate_display(name, orientation):
    dm = get_current_mode(name)
    if dm.dmDisplayOrientation == orientation:
        print(f"schon in Orientierung {orientation}, nichts zu tun")
        return 0
    dm.dmPelsWidth, dm.dmPelsHeight = dm.dmPelsHeight, dm.dmPelsWidth  # Wechsel quer <-> hoch
    dm.dmDisplayOrientation = orientation
    dm.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT
    return apply_mode(name, dm, f"Orientierung {orientation}")


def set_display_mode(name, width, height):
    dm = get_current_mode(name)
    dm.dmPelsWidth, dm.dmPelsHeight = width, height
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT
    return apply_mode(name, dm, f"Modus {width}x{height}")


if __name__ == "__main__":
    name, beschreibung = find_virtual_display()
    dm = get_current_mode(name)
    print(f"{name} ({beschreibung}): {dm.dmPelsWidth}x{dm.dmPelsHeight}, Orientierung {dm.dmDisplayOrientation}")
    if len(sys.argv) > 1:
        befehl = sys.argv[1]
        if befehl == "modes":
            for breite, hoehe, hz in list_modes(name):
                print(f"  {breite}x{hoehe} @ {hz} Hz")
        elif "x" in befehl:
            breite, hoehe = (int(wert) for wert in befehl.split("x"))
            sys.exit(set_display_mode(name, breite, hoehe))
        else:
            sys.exit(rotate_display(name, DMDO_90 if befehl.startswith("p") else DMDO_DEFAULT))
