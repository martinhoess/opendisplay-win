# opendisplay-win

Use an iPad as a second monitor for **Windows**, over Wi-Fi or USB, driving the
**unmodified** [OpenDisplay](https://github.com/peetzweg/opendisplay) receiver
app on the iPad. OpenDisplay ships a macOS sender only; this project
reimplements that sender side for Windows and speaks the iPad's exact wire
protocol, so the iOS app needs no changes.

> **Status: prototype.** It works end to end: real desktop on the iPad, finger
> touch, Apple Pencil with pressure and tilt, live cursor, Wi-Fi or USB, iPads
> found on the network by themselves. It is still not a polished product: the
> binary is unsigned, the virtual display comes from a third-party driver you
> install yourself, and the link is unencrypted (see the [ROADMAP](ROADMAP.md)).
> Expect rough edges.

MTT support, the USB transport, pause mode and the reworked encoder pipeline
were contributed by Michael Sciscenti ([@mshish](https://github.com/mshish)) in
[pull request #1](https://github.com/martinhoess/opendisplay-win/pull/1).

## How it works

The iPad app listens on TCP port 9000; the Windows sender connects to it and:

1. **Virtual monitor**: creates a headless display sized to the iPad's panel,
   so Windows sees a real second monitor you can drag windows onto. Two drivers
   can provide it: the
   [MTT Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
   (one iPad at a time, a fixed monitor identity so Windows remembers where you
   put it) or [parsec-vdd](https://github.com/nomi-san/parsec-vdd) (several
   iPads at once).
2. **Transport**: connects over Wi-Fi, or over the USB cable through Apple's
   usbmuxd. If the cable drops it can carry on over Wi-Fi, and it moves back to
   USB when the cable is plugged in again. iPads running OpenDisplay are found
   over Bonjour (`_opensidecar._tcp`), so you can add them from a list instead
   of typing an address.
3. **Capture**: grabs that monitor with DXGI Desktop Duplication. The cursor is
   not painted into the picture; its shape and position go to the iPad
   separately (positions over a UDP side channel), so it moves even when
   nothing is being encoded.
4. **Encode**: H.264 via Media Foundation. Intel Quick Sync is tried first,
   then other hardware encoders (NVIDIA, AMD, others), then Microsoft's
   software encoder. The output is an Annex-B byte stream normalized to the
   exact framing the iPad decoder expects (4-byte start codes, SPS/PPS on every
   keyframe).
5. **Stream**: one length-prefixed message per frame over the TCP connection.
   The frame rate is capped at what the receiver announces in its `hello`, and
   a frame is only encoded when the screen changed (an idle desktop drops to
   about one keepalive frame per second).
6. **Input**: the iPad's touch and scroll events come back on the same
   connection. Fingers are injected as Windows touch (a synthetic `PT_TOUCH`
   pointer), scrolling as mouse wheel, and Apple Pencil as a real Windows pen
   (`PT_PEN`) carrying pressure, tilt and hover, all mapped onto the virtual
   monitor's rectangle.

It reconnects on its own if the iPad app is closed/reopened or the link drops,
and rebuilds the pipeline when the iPad sends a new size (for example after a
rotation).

**Pause mode.** When Windows locks, the display turns off, or a UAC prompt
switches to the secure desktop, the connection stays up and the iPad shows a
card instead of the desktop. See [Pause mode](#pause-mode).

### Deliberately out of scope

H.265 and audio. H.264 is required, the iPad receiver is hardcoded to it.

Limits that remain: MTT drives one iPad at a time (use Parsec for several);
iPads with different panel sizes never stream at the same time on either
driver; the stock receiver turns multi-finger gestures into scrolling, so true
multi-touch would need a receiver change.

## Install

Needs Windows 10/11 (x64) and, on the iPad, the
[OpenDisplay](https://github.com/peetzweg/opendisplay) receiver app.

### 1. Install a display driver

Both are **separate third-party drivers** and neither is bundled with the
release. Without one there is no virtual monitor to capture, and the app will
not connect. Install one of them (or both) once, as administrator.

**MTT Virtual Display Driver**

1. From the
   [Virtual-Display-Driver releases](https://github.com/VirtualDrivers/Virtual-Display-Driver/releases)
   download Virtual Driver Control (`VDD.Control.<version>.zip`), extract it,
   run it and click **Install Driver**.
2. The driver keeps its settings in `C:\VirtualDisplayDriver`. opendisplay-win
   writes two files there: `vdd_settings.xml` (the mode list, `CustomEdid`,
   `PreventSpoof` and, if it was 0, the monitor count set to 1) and
   `user_edid.bin` (an EDID generated for your iPad's panel). Before the first
   write it makes a one-time backup of each as `.bak`, so the original driver
   config can be put back by hand.
3. Admin rights are needed only when the iPad's size differs from what is
   installed: the first iPad, or a different model later. The tray shows a
   notice, Windows asks once, and the app then restarts the driver (the
   `Root\MttVDD` adapter) so it picks up the new modes. The same iPad
   reconnecting asks nothing. If you decline, the iPad still connects with the
   EDID that is already installed.

The window position and mode you choose for the MTT monitor in Windows Display
Settings are saved to `%APPDATA%\opendisplay-win\mtt_display.json` and restored
the next time the monitor is attached. opendisplay-win only removes an MTT
monitor it created itself; heads from other tools are left alone.

**parsec-vdd**

1. Download `parsec-vdd-0.45.0.0.exe` from `https://builds.parsec.app/vdd/`
   and run it (silent install: `/S`).
2. Run `"C:\Program Files\Parsec Virtual Display Driver\vddinstall.bat"`
   **elevated** (right-click → *Run as administrator*, or from an elevated
   terminal). This step is **not optional**: the installer in step 1 only
   extracts files, and this script does the actual `nefconw` device-node
   creation. Running the installer with `/S` alone leaves you with a driver
   that is present but has no device node.
3. Verify: open Device Manager (`devmgmt.msc`) → **Display adapters** → an
   entry named **"Parsec Virtual Display Adapter"** must be listed. If it is
   missing, step 2 did not run elevated; repeat it.

The driver stays installed and survives reboots; you never repeat this.

> To remove it later: uninstall "Parsec Virtual Display Driver" via
> *Settings → Apps*, which also drops the device node.

**Choosing between them.** *Settings → Advanced → Display driver* offers
**Auto**, **MTT (1 iPad)** and **Parsec (multiple iPads)** (`displayBackend` in
the config: `auto`, `mtt`, `parsec`). Auto uses MTT when its driver is
installed, otherwise Parsec. If the driver you picked is missing, the iPad does
not connect, its menu entry reads *Display driver missing* and a balloon says
so; the app never switches to the other driver on its own. Changing the setting
reconnects every iPad.

### 2. USB (optional)

USB goes through Apple's usbmuxd, which comes with the **Apple Devices** app or
**iTunes**; one of them has to be installed. The iPad must trust this PC, and
OpenDisplay must be open on it.

There is nothing to configure per iPad. An entry uses the cable once it knows
the iPad's id: entries added from Nearby have it, and a typed-in address learns
it from the first Wi-Fi connection (adding the same iPad from Nearby later also
fills it in). Until then the entry stays on Wi-Fi. *Stay connected over Wi-Fi
if the USB cable drops* (Settings, on by default) keeps the session alive when
you unplug; plugging back in moves it to USB again. The menu entry shows which
link is in use: *Connected (USB)* or *Connected (Wi-Fi)*. If usbmuxd stops
listing the iPad (trust prompt, locked iPad), the entry uses Wi-Fi until it
reappears.

### 3. Download opendisplay-win.exe

Grab `opendisplay-win.exe` from the
[latest release](https://github.com/martinhoess/opendisplay-win/releases/latest).
It is a single self-contained executable: no installer, no VC++
Redistributable, no DLLs alongside it. Put it wherever you like (it writes its
config to `%APPDATA%`, not next to itself).

**SmartScreen will warn you.** The binary is not code-signed yet, so Windows
shows *"Windows protected your PC"* on first launch. Click **More info** →
**Run anyway**. Signing is planned via the SignPath Foundation's free OSS
programme (see the [ROADMAP](ROADMAP.md)); until that comes through, every
release is unsigned. If you would rather not trust an unsigned binary from a
stranger on the internet: [build it from source](#build-from-source), it is two
commands.

### 4. First start

Double-click the exe. It starts as a tray app (see [Run](#run)); open
*Settings* from its right-click menu. The **General** tab lists the iPads found
on your network under **Nearby** (OpenDisplay has to be open on the iPad, on
the same Wi-Fi); select one and click **Add >** to move it to **Your iPads**.
**Refresh** searches again. To enter an address by hand, use **Manual add /
rename** on the **Advanced** tab: one iPad per line, the address followed by an
optional nickname (`192.168.1.42 Living Room`).

The menu names the iPads by what the receiver advertises over Bonjour, or, when
that is still the generic "iPad" iOS hands out, by the device's host name
(`iPad-Pro.local` → *iPad-Pro*). A nickname you set wins. An entry added from
Nearby is remembered by the iPad's id, so it is found again when DHCP gives the
iPad a new address.

**One panel size at a time.** iPads with *different* panels do not stream
together on either driver. On Parsec the driver puts a single custom resolution
on all of its virtual monitors, so iPads of the same size stream together and a
different one waits: its menu entry reads *Waiting for 2732x2048* until the
iPad holding the display disconnects, then it starts on its own. On MTT a
second iPad always waits, with *Waiting - screen in use by <name>* and a
balloon, and connects when the first one disconnects.

The first time you connect a **new** iPad size, Windows asks for admin once
(details in [Admin rights](#admin-rights) below). Accept it; the same iPad
never asks again.

**Parsec: expect leftover monitors in Device Manager.** parsec-vdd mints a
fresh monitor identity on every add and leaves the device node behind when the
app exits, so roughly one phantom "Parsec Virtual Display" piles up per run.
They are harmless but they accumulate; clear them out now and then with

```
opendisplay-win.exe --cleanup-monitors
```

This is deliberately not automatic, removing devices from inside the runtime
path is too invasive. MTT keeps one fixed monitor identity and does not have
this problem.

### Settings

- **General**: Nearby / Your iPads (above); *Connect automatically when
  OpenDisplay is running*; *Stay connected over Wi-Fi if the USB cable drops*;
  *Start with Windows*; *Streaming quality* (**Faster response**, **Balanced**,
  **Sharper**).
- **Advanced**: Manual add / rename; *Port*; *Display driver*; *Separate USB
  and Wi-Fi quality settings* (each with a **Dynamic**/**Fixed** mode and a
  **Speed**/**Balanced**/**Quality** preset); *Restart as administrator*.

### Admin rights

The app **runs un-elevated**. What needs admin depends on the driver, and both
cases self-elevate a one-off helper (a single UAC prompt) and then carry on
un-elevated:

- **MTT**: writing `vdd_settings.xml` / `user_edid.bin` under
  `C:\VirtualDisplayDriver` and restarting the driver, only when the iPad's
  size changes (and once if the driver's monitor count is 0). By hand, from an
  elevated terminal: `opendisplay-win.exe --ensure-mtt-edid <width>x<height>`.
- **Parsec**: registering an iPad's native resolution (both orientations) as a
  parsec-vdd custom mode, one `HKLM` write per panel size. By hand:

```
build\Release\opendisplay-win.exe --register-resolution <width> <height>
```

Running the whole app as administrator is **optional** (Settings → Advanced →
*Restart as administrator*) and only matters if you want touch to control
*elevated* windows on the iPad screen; Windows blocks input from an un-elevated
process into higher-integrity windows (UIPI).

### Pause mode

Locking Windows (Win+L), the display turning off, or a UAC prompt no longer
drops the connection. Capture and encoding stop, the link stays up, and the
iPad shows a card saying how the pause ends:

- **Locked**: unlock to continue.
- **Waiting for admin approval**: a UAC prompt is on the secure desktop.
- **Display asleep**: wake the display to continue.
- **Paused**: any other moment the desktop cannot be captured; resumes
  automatically.

While locked, on the UAC desktop or asleep, no desktop content reaches the
iPad: the card is drawn on a near-black background and the last frame is
discarded. A touch on the iPad wakes the display. When the pause ends, capture
is rebuilt and a fresh keyframe is sent.

### Start with Windows

Because the app is un-elevated, autostart is just a per-user Run entry: toggle
**Start with Windows** in Settings (no scheduled task, no logon UAC).

## Build from source

Needs Visual Studio Build Tools (Desktop C++ workload) or Visual Studio with
the Windows 10/11 SDK, and CMake ≥ 3.20. A display driver from
[Install](#1-install-a-display-driver) is required to *run* it, not to build
it.

```
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Release
```

The result is `build\Release\opendisplay-win.exe`, statically linked against
the CRT, the same single-file binary the releases ship. The build also copies
`assets\mtt\user_edid.bin` next to it: a fallback EDID for MTT, used only when
nothing is installed under `C:\VirtualDisplayDriver` yet and the admin prompt
was declined.

## Run

The commands below spell out the build-tree path; if you downloaded the release
binary, substitute wherever you put `opendisplay-win.exe`.

Launched **without arguments** it runs as a **tray app** (one instance per
Windows session): a notification-area icon whose status dot is green when at
least one iPad is streaming, red when configured but none is, grey when no iPad
is set. The right-click menu lists every configured iPad with its state
(*Connected (USB)*, *Connected (Wi-Fi)*, *Connecting...*, *Waiting for
2266x1488*, *Waiting - screen in use by <name>*, *Display driver missing*,
*Not connected*). Click one to connect or disconnect just that one; the check
mark means it is streaming. Below that: Connect/Disconnect (all), Settings and
Quit. Config is saved to `%APPDATA%\opendisplay-win\config.json`; a log goes to
`log.txt` next to it, with each line tagged by the iPad it belongs to, and the
previous run's log is kept as `log.prev.txt`.

```
build\Release\opendisplay-win.exe
```

Launched **with an IP** it runs **headless** (no UI), handy for testing/scripting.
Each headless instance logs to its own `log-<pid>.txt`, so several can run side
by side without overwriting each other's log:

```
build\Release\opendisplay-win.exe <ipad-ip>
```

Either way the iPad receiver app must already be running (listening on port
9000); the sender reconnects automatically on drops.

One-off commands print their result in the terminal that started them, or to
`log-<pid>.txt` when there is no console.

`--probe-encoder <width> <height>` picks the encoder for that size the way a
connection would and feeds it gray frames until a keyframe comes out. It tells
"no encoder accepts this panel" apart from a capture or network problem,
without an iPad:

```
build\Release\opendisplay-win.exe --probe-encoder 2732 2048
```

`--render-pause-previews <dir> [<width>x<height>]` writes the four pause cards
over a sample desktop as PNGs (`overlay-locked.png`, `overlay-asleep.png`,
`overlay-admin.png`, `overlay-paused.png`) into an existing folder; the size
defaults to 2360x1640. It reports to `log-<pid>.txt`.

`--cleanup-monitors` (Parsec) removes phantom virtual-monitor devices left in
Device Manager by earlier runs (see the ROADMAP for why they pile up):

```
build\Release\opendisplay-win.exe --cleanup-monitors
```

`--remove-display <index>` (Parsec) unplugs a virtual monitor that is still
*attached* because a sender was killed rather than shut down. The driver keeps
such a display on the desktop for as long as any client holds the adapter
open, and `--cleanup-monitors` only touches devices that are already gone.
Indexes are driver-global and count from 0 in the order the displays were
added, so only run this while the senders are stopped:

```
build\Release\opendisplay-win.exe --remove-display 1
```

## Testing without an iPad

`tools/mock_receiver.py` (Python 3, stdlib only) stands in for the iPad:
listens on :9000, sends `hello`, logs control messages, and structurally
validates every video payload against the Annex-B framing rules (4-byte start
codes only, SPS/PPS immediately before each keyframe). Handy for exercising
the transport, encoder byte format, reconnect and rotation paths locally.

```
python3 tools/mock_receiver.py --width 2388 --height 1668
# then, in another terminal:
build\Release\opendisplay-win.exe 127.0.0.1
```

`--rotate-after N` sends a second `hello` with the panel dimensions swapped
after N frames, i.e. what the iPad sends when it is turned.

Two iPads at once can be faked over the loopback range (Parsec driver): bind
one mock to `127.0.0.1` and another to `127.0.0.2` (`--host`), then start a
sender against each. Both senders must end up on **different** `\\.\DISPLAYn`
monitors; the `claimed virtual monitor` line in the log says which.

`tools/gen_user_edid.py` is the reference generator for the MTT EDID; see
[assets/mtt/README.md](assets/mtt/README.md).

## License

GPL-3.0-or-later, a derivative of OpenDisplay (GPL-3.0). See [LICENSE](LICENSE).
`third_party/parsec-vdd/parsec-vdd.h` is vendored unmodified from
[nomi-san/parsec-vdd](https://github.com/nomi-san/parsec-vdd) (MIT, GPL-3.0
compatible).
