# Roadmap

Where this prototype could go to become a real, distributable product.
Nothing here is committed to; it's a map of the work and, more importantly, the
one strategic decision everything else hangs off.

## The linchpin: driver code-signing

A self-contained virtual monitor means shipping **our own Indirect Display
Driver (IddCx)**, and a Windows driver must be code-signed to install without
test-signing mode (which is an unacceptable end-user experience). This is the
real cost and the gating decision; the upstream maintainer independently
reached the same conclusion in
[opendisplay#65](https://github.com/peetzweg/opendisplay/issues/65).

| Path | Cost | Effort |
|------|------|--------|
| **SignPath Foundation**: free code signing for OSS | €0 (we are GPL-3.0 OSS) | application + review, external/slow |
| EV cert + Microsoft Partner Center (attestation) | ~€300-400/yr | medium |

The Virtual-Display-Driver (MTT) is signed exactly this way (SignPath).
**Recommendation: apply for SignPath OSS signing early.** It's the long pole
(external approval); everything else can be developed locally under
test-signing in parallel.

## 1. Own virtual display (drop the third-party drivers)

Today the sender drives one of two third-party drivers, both installed
out-of-band by the user: the **MTT Virtual Display Driver** or **parsec-vdd**
(`nefconw` + registry dance).

**The shortcut is taken.** MTT gives the monitor a fixed identity (a generated
EDID per iPad, same manufacturer/product/serial every time), so Windows keeps
its arrangement, and the sender saves and restores the layout itself. No
phantom monitors pile up and no UID churn. What MTT does not solve:

- It drives **one iPad at a time**. Several iPads still need Parsec, with its
  shared custom resolution and fresh UID on every add.
- Changing the panel size means rewriting the driver's files under
  `C:\VirtualDisplayDriver` and restarting the adapter, which needs admin.
- It is still a separate download and install step.

An own driver remains the clean fix:

- **Fork the Microsoft IddCx sample / Virtual-Display-Driver** (MIT), rebrand,
  bundle it. Do **not** write one from scratch; the sample is 90% of the work.
- One monitor per iPad, each with its own EDID and stable identity; modes
  could be set per monitor without an admin-only file rewrite, and the display
  tied to the client handle so a killed sender cannot leave it plugged in.
- Removes the external install step.
- Effort: **M** (driver code) + external (signing, see above).

## 2. Background / service model

**A naive Windows service will not work here.** Services run in session 0,
isolated: from there you can do **neither Desktop Duplication of the user's
session nor `SendInput`** into it. A "proper" service needs a split: a session-0
service (lifecycle) **plus** an interactive agent launched into the user session
via `WTSQueryUserToken` / `CreateProcessAsUser` (how Sunshine / Parsec / RDP do
it).

**Largely done.** The app doesn't need elevation at all. The only
admin-requiring steps are one-offs that self-elevate: registering an iPad
resolution as a parsec-vdd custom mode (one HKLM write), or writing a new
iPad's EDID for MTT and restarting that driver. So the tray app runs
un-elevated and **autostart is a plain per-user Run entry** (Settings → *Start
with Windows*), no scheduled task and no logon UAC. Locking the PC, display
sleep and UAC prompts pause the stream instead of dropping it. The full
session-0 service + interactive agent split is only worth it if it must run
*before* user logon / fully headless, not needed for "a second monitor for my
own PC".

## 3. Tray app + settings GUI: done

- Native Win32 tray app (`Shell_NotifyIcon`) that *is* the sender host: a
  status-dot icon, one entry per iPad with its state, Connect/Disconnect,
  Settings and Quit.
- Settings with General / Advanced tabs: Nearby pick list, quality presets,
  display driver choice, USB failover, Restart as administrator.
- `SenderApp` is controllable (Start/Stop/state); config persists as JSON in
  `%APPDATA%`. Tray status is pushed by sender events, not a timer.
- Remaining polish: async Stop so the UI never briefly blocks; a richer
  C#/WinUI front-end is possible later but not needed.

## 4. Installer / distribution

Depends on item 1 (a signed driver).

- Tooling: **Inno Setup** (simpler than WiX for "app + driver + auto-start
  task"). **MSIX is out**, it can't cleanly install drivers.
- Steps: lay down the app, `pnputil /add-driver /install` the signed driver,
  register the auto-start task, create shortcuts, and an uninstaller that also
  removes the virtual display + driver. Sign the installer with the same cert.

## Additional ideas

Some from the [#65](https://github.com/peetzweg/opendisplay/issues/65)
component-mapping, some ours. Done items first:

- ~~**mDNS/Bonjour discovery**~~: **done.** `src/net/Mdns.cpp` (a hand-rolled
  query, no Bonjour SDK) listens for `_opensidecar._tcp` announcements and
  re-browses on network change or when the menu or Settings opens. Settings
  offers the found receivers as a pick list, and an entry with the receiver's
  TXT `id` follows an address that DHCP moved.
  **The trap when testing this:** left to Windows, a multicast query follows the
  route metric, and on a machine with VMware or Hyper-V adapters it leaves
  through one of those and never reaches the iPads, with no error, just
  silence. Discovery therefore runs on *every* IPv4 interface.
- ~~**USB transport**~~: **done.** An own usbmux client (`src/net/UsbMux.cpp`)
  talks to the usbmuxd that ships with Apple Devices / iTunes; no
  libimobiledevice. Each entry dials its own iPad, matched by the id learned
  from the iPad's `hello`, with optional Wi-Fi failover and an automatic move
  back to USB on the attach event.
- ~~**Multiple iPads at once**~~: **done, with restrictions.** The tray runs
  one `SenderApp` per configured iPad, each with its own virtual monitor,
  capture/encode pipeline and input injection. On Parsec several iPads stream
  at once, but parsec-vdd puts one custom resolution on *all* of its virtual
  monitors, so only same-size iPads stream together and a different one waits
  (`SenderApp::State::Blocked`). MTT drives one iPad; a second waits until the
  first disconnects. Lifting both is item 1.
- ~~**Encoder fallback / GPU path**~~: **done.** Intel Quick Sync first, then
  other hardware MFTs, then Microsoft's software encoder; event-driven async
  MFTs; the capture's D3D11 device is shared with any hardware MFT that accepts
  it. The CPU BGRA→NV12 conversion is only the fallback for encoders that
  don't.
- **⚠️ Security**: the link is currently **unencrypted and unauthenticated**:
  anyone on the LAN can connect, see the screen, **and inject input** (i.e.
  remote-control the PC). A real blocker for public distribution. Add pairing
  (PIN) and optionally TLS *before* shipping widely.
- **Multi-touch**: the stock receiver collapses multi-finger input to scroll;
  real multi-touch needs a receiver change.
- **Clock-sync / latency overlay**: the telemetry prefix + `pong` reply we
  currently skip would light up the iPad's latency overlay. Nice-to-have.
- **Smaller items**: adaptive bitrate; GPU device-lost / multi-GPU robustness;
  auto-update.

## Known issues & smaller open items

- **Parsec: a killed sender leaves its monitor plugged in.** Not just a devnode:
  the display stays *attached* to the desktop, because the driver only unplugs
  on an explicit `VddRemoveDisplay` (which a `taskkill /F` skips) or once no
  client holds the adapter open at all, and any other running sender keeps it
  open. `--remove-display <index>` unplugs one by index; run it with the
  senders stopped.
- **Parsec: phantom virtual monitors accumulate ~1 per run.** parsec-vdd mints
  a fresh monitor UID on every add and, when the process dies, unplugs it but
  leaves the devnode behind. `opendisplay-win.exe --cleanup-monitors` removes
  the leftovers on demand. Deliberately **not** run automatically, device
  removal is too invasive for the runtime path. MTT does not have this problem.
- **Keyframe cadence spikes under motion.** Seen with NVENC on the old
  pipeline: heavy on-screen change emitted far more IDR frames than the GOP
  implies (scene-change I-frames). Bandwidth only, not correctness; not
  re-measured since the encoder rework, folds into the adaptive-bitrate item
  above.

## Suggested order

1. **Apply for SignPath OSS signing**: the long pole; gates 1 and 4.
2. **Own IddCx driver** (1): develop locally under test-signing while signing
   is pending.
3. **Installer** (4): once the driver is signed.
4. **Security** (pairing / TLS): before any public distribution.
