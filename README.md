# opendisplay-win

Use an iPad as a wireless second monitor for **Windows**, driving the
**unmodified** [OpenDisplay](https://github.com/peetzweg/opendisplay) receiver
app on the iPad. OpenDisplay ships a macOS sender only; this project
reimplements that sender side for Windows and speaks the iPad's exact wire
protocol, so the iOS app needs no changes.

> **Status: proof-of-concept / prototype.** It works end to end — real desktop
> on the iPad, touch and scroll driving the mouse, live cursor — but it is a
> single-purpose spike, not a polished product: WiFi only, one iPad, manual IP
> entry, hardcoded stream settings, a minimal tray UI. Expect rough edges.

## How it works

The iPad app listens on TCP port 9000; the Windows sender connects to it and:

1. **Virtual monitor** — creates a headless display sized to the iPad's panel
   via the [parsec-vdd](https://github.com/nomi-san/parsec-vdd) driver, so
   Windows sees a real second monitor you can drag windows onto.
2. **Capture** — grabs that monitor with DXGI Desktop Duplication and
   composites the mouse cursor into each frame (Desktop Duplication delivers the
   cursor out-of-band; color, masked-color and monochrome shapes are handled).
3. **Encode** — H.264 via Media Foundation (hardware NVENC/QuickSync/AMF when
   available), emitting an Annex-B byte stream normalized to the exact framing
   the iPad decoder expects (4-byte start codes, SPS/PPS on every keyframe).
4. **Stream** — one length-prefixed message per frame over the single TCP
   connection, only re-encoding when the screen or cursor actually changes (an
   idle desktop drops to ~1 keepalive frame/second).
5. **Input** — the iPad's touch and scroll events come back on the same
   connection and are injected as mouse input with `SendInput`, mapped onto the
   virtual monitor's rectangle.

It reconnects on its own if the iPad app is closed/reopened or the link drops,
and rebuilds the pipeline if the iPad rotates (the panel dimensions change).

### Deliberately out of scope

USB (usbmuxd), mDNS/Bonjour discovery, pen/pressure input, H.265, audio, and
multiple simultaneous iPads. H.264 is required — the iPad receiver is
hardcoded to it.

## Prerequisites

- Windows 10/11
- Visual Studio Build Tools (Desktop C++ workload) or Visual Studio, with the
  Windows 10/11 SDK; CMake ≥ 3.20
- The [parsec-vdd](https://github.com/nomi-san/parsec-vdd) virtual display
  driver, installed once (needs admin rights):
  1. Download & run `parsec-vdd-0.45.0.0.exe` from `https://builds.parsec.app/vdd/` (silent: `/S`)
  2. Run `"C:\Program Files\Parsec Virtual Display Driver\vddinstall.bat"` **elevated**
     — the installer only extracts files; this script does the actual
     `nefconw` device-node creation (`/S` alone is not enough)
  3. Confirm "Parsec Virtual Display Adapter" appears in Device Manager

The sender **runs as Administrator** — it writes the driver's custom-resolution
registry key under `HKLM\SOFTWARE\Parsec\vdd` (fails silently without
elevation). The binary's manifest requests elevation, so launching it triggers
a UAC prompt.

## Build

```
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Release
```

## Run

Launched **without arguments** it runs as a **tray app**: a notification-area
icon whose status dot is green when connected, red when configured but not, grey
when no iPad is set. Right-click for Connect/Disconnect/Settings/Exit; Settings
holds the iPad IP, port, and auto-connect. Config is saved to
`%APPDATA%\opendisplay-win\config.json`; a log goes to `log.txt` next to it.

```
build\Release\opendisplay-win.exe
```

Launched **with an IP** it runs **headless** (no UI), handy for testing/scripting:

```
build\Release\opendisplay-win.exe <ipad-ip>
```

Either way the iPad receiver app must already be running (listening on port
9000); the sender reconnects automatically on drops.

`--cleanup-monitors` is a one-off that removes phantom virtual-monitor devices
left in Device Manager by earlier runs (see the ROADMAP for why they pile up):

```
build\Release\opendisplay-win.exe --cleanup-monitors
```

## Testing without an iPad

`tools/mock_receiver.py` (Python 3, stdlib only) stands in for the iPad:
listens on :9000, sends `hello`, logs control messages, and structurally
validates every video payload against the Annex-B framing rules (4-byte start
codes only, SPS/PPS immediately before each keyframe) — handy for exercising
the transport, encoder byte format, reconnect and rotation paths locally.

```
python3 tools/mock_receiver.py --width 2388 --height 1668
# then, in another terminal:
build\Release\opendisplay-win.exe 127.0.0.1
```

## License

GPL-3.0-or-later — a derivative of OpenDisplay (GPL-3.0). See [LICENSE](LICENSE).
`third_party/parsec-vdd/parsec-vdd.h` is vendored unmodified from
[nomi-san/parsec-vdd](https://github.com/nomi-san/parsec-vdd) (MIT, GPL-3.0
compatible).
