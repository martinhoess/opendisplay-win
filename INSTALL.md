# Installing opendisplay-win

Needs Windows 10/11 (x64) and, on the iPad, the
[OpenDisplay](https://github.com/peetzweg/opendisplay) receiver app.

## 1. Install a display driver

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

## 2. USB (optional)

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

## 3. Download opendisplay-win.exe

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
stranger on the internet: [build it from source](README.md#build-from-source), it is two
commands.

## 4. First start

Double-click the exe. It starts as a tray app (see [Run](README.md#run)); open
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

## Admin rights

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

## Start with Windows

Because the app is un-elevated, autostart is just a per-user Run entry: toggle
**Start with Windows** in Settings (no scheduled task, no logon UAC).
