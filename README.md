# opendisplay-win

Use an iPad as a wireless second monitor for **Windows**, driving the
**unmodified** [OpenDisplay](https://github.com/peetzweg/opendisplay) receiver
app on the iPad. OpenDisplay ships a macOS sender only; this project
reimplements that sender side for Windows and speaks the iPad's exact wire
protocol, so the iOS app needs no changes.

> **Status: proof-of-concept / prototype.** It works end to end — real desktop
> on the iPad, touch and scroll driving the mouse, Apple Pencil with pressure
> and tilt, live cursor — but it is a
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
   virtual monitor's rectangle. Apple Pencil is injected separately, as a real
   Windows pen pointer (`InjectSyntheticPointerInput`) carrying pressure, tilt
   and hover, so pressure-aware apps see a pen rather than a mouse.

It reconnects on its own if the iPad app is closed/reopened or the link drops,
and rebuilds the pipeline if the iPad rotates (the panel dimensions change).

### Deliberately out of scope

USB (usbmuxd), mDNS/Bonjour discovery, H.265, audio, and multiple simultaneous
iPads. H.264 is required — the iPad receiver is hardcoded to it.

## Install

Needs Windows 10/11 (x64) and, on the iPad, the
[OpenDisplay](https://github.com/peetzweg/opendisplay) receiver app.

### 1. Install the parsec-vdd driver

This is a **separate third-party driver** and is not bundled with the release —
without it there is no virtual monitor to capture, and the app will refuse to
connect. Install it once, as administrator:

1. Download `parsec-vdd-0.45.0.0.exe` from `https://builds.parsec.app/vdd/`
   and run it (silent install: `/S`).
2. Run `"C:\Program Files\Parsec Virtual Display Driver\vddinstall.bat"`
   **elevated** — right-click → *Run as administrator*, or from an elevated
   terminal. This step is **not optional**: the installer in step 1 only
   extracts files, and this script does the actual `nefconw` device-node
   creation. Running the installer with `/S` alone leaves you with a driver
   that is present but has no device node.
3. Verify: open Device Manager (`devmgmt.msc`) → **Display adapters** → an
   entry named **"Parsec Virtual Display Adapter"** must be listed. If it is
   missing, step 2 did not run elevated — repeat it.

The driver stays installed and survives reboots; you never repeat this.

> To remove it later: uninstall "Parsec Virtual Display Driver" via
> *Settings → Apps*, which also drops the device node.

### 2. Download opendisplay-win.exe

Grab `opendisplay-win.exe` from the
[latest release](https://github.com/martinhoess/opendisplay-win/releases/latest).
It is a single self-contained executable — no installer, no VC++
Redistributable, no DLLs alongside it. Put it wherever you like (it writes its
config to `%APPDATA%`, not next to itself).

**SmartScreen will warn you.** The binary is not code-signed yet, so Windows
shows *"Windows protected your PC"* on first launch. Click **More info** →
**Run anyway**. Signing is planned via the SignPath Foundation's free OSS
programme (see the [ROADMAP](ROADMAP.md)) — until that comes through, every
release is unsigned. If you would rather not trust an unsigned binary from a
stranger on the internet: [build it from source](#build-from-source), it is two
commands.

### 3. First start

Double-click the exe. It starts as a tray app (see [Run](#run)); open
*Settings* from its right-click menu and enter the iPad's IP address.

The first time you connect a **new** iPad, you get **one UAC prompt** — the app
registers that panel's resolution as a parsec-vdd custom mode, which needs a
single `HKLM` write. Accept it; it happens once per iPad model, never again.
Details in [Admin rights](#admin-rights) below.

**Expect leftover monitors in Device Manager.** parsec-vdd mints a fresh
monitor identity on every add and leaves the device node behind when the app
exits, so roughly one phantom "Parsec Virtual Display" piles up per run. They
are harmless but they accumulate; clear them out now and then with

```
opendisplay-win.exe --cleanup-monitors
```

This is deliberately not automatic — removing devices from inside the runtime
path is too invasive. See the [ROADMAP](ROADMAP.md) for why an own driver would
remove the cause.

### Admin rights

The app **runs un-elevated**. The *only* thing that needs admin is registering
an iPad's native resolution as a parsec-vdd custom mode — one `HKLM` write, done
once per panel size. The first time you connect a new iPad, the app self-elevates
a one-off (a single UAC prompt) to register both orientations, then keeps running
un-elevated; after that it never prompts again. You can also do it by hand:

```
build\Release\opendisplay-win.exe --register-resolution <width> <height>
```

Running the whole app as administrator is **optional** (tray menu → *Run as
administrator*) and only matters if you want touch to control *elevated* windows
on the iPad screen — Windows blocks input from an un-elevated process into
higher-integrity windows (UIPI).

### Start with Windows

Because the app is un-elevated, autostart is just a per-user Run entry — toggle
**Start with Windows** in Settings (no scheduled task, no logon UAC).

## Build from source

Needs Visual Studio Build Tools (Desktop C++ workload) or Visual Studio with
the Windows 10/11 SDK, and CMake ≥ 3.20. The parsec-vdd driver from
[Install](#1-install-the-parsec-vdd-driver) is required to *run* it, not to
build it.

```
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Release
```

The result is `build\Release\opendisplay-win.exe`, statically linked against
the CRT — the same single-file binary the releases ship.

## Run

The commands below spell out the build-tree path; if you downloaded the release
binary, substitute wherever you put `opendisplay-win.exe`.

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
