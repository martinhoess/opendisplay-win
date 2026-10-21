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
[OpenDisplay](https://github.com/peetzweg/opendisplay) receiver app. In short:

1. Install a display driver: MTT for one iPad, parsec-vdd for several.
2. Optional, for USB: Apple Devices or iTunes.
3. Download `opendisplay-win.exe` from the
   [latest release](https://github.com/martinhoess/opendisplay-win/releases/latest)
   and start it; it runs in the tray.
4. Open *Settings* and add your iPad from **Nearby**.

The steps in detail, including admin rights and autostart, are in
[INSTALL.md](INSTALL.md).

## Settings

- **General**: Nearby / Your iPads (see
  [First start](INSTALL.md#4-first-start)); *Connect automatically when
  OpenDisplay is running*; *Stay connected over Wi-Fi if the USB cable drops*;
  *Start with Windows*; *Streaming quality* (**Faster response**, **Balanced**,
  **Sharper**).
- **Advanced**: Manual add / rename; *Port*; *Display driver*; *Separate USB
  and Wi-Fi quality settings* (each with a **Dynamic**/**Fixed** mode and a
  **Speed**/**Balanced**/**Quality** preset); *Restart as administrator*.

## Pause mode

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

## Build from source

Needs Visual Studio Build Tools (Desktop C++ workload) or Visual Studio with
the Windows 10/11 SDK, and CMake ≥ 3.20. A display driver from
[Install](INSTALL.md#1-install-a-display-driver) is required to *run* it, not to build
it.

```
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Release
```

The result is `build\Release\opendisplay-win.exe`, statically linked against
the CRT, the same single-file binary the releases ship.

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
[tools/gen_user_edid.md](tools/gen_user_edid.md).

## License

GPL-3.0-or-later, a derivative of OpenDisplay (GPL-3.0). See [LICENSE](LICENSE).
`third_party/parsec-vdd/parsec-vdd.h` is vendored unmodified from
[nomi-san/parsec-vdd](https://github.com/nomi-san/parsec-vdd) (MIT, GPL-3.0
compatible).
