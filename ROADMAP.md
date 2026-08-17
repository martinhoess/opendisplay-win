# Roadmap

Where this proof-of-concept could go to become a real, distributable product.
Nothing here is committed to; it's a map of the work and, more importantly, the
one strategic decision everything else hangs off.

## The linchpin: driver code-signing

A self-contained virtual monitor means shipping **our own Indirect Display
Driver (IddCx)**, and a Windows driver must be code-signed to install without
test-signing mode (which is an unacceptable end-user experience). This is the
real cost and the gating decision — the upstream maintainer independently
reached the same conclusion in
[opendisplay#65](https://github.com/peetzweg/opendisplay/issues/65).

| Path | Cost | Effort |
|------|------|--------|
| **SignPath Foundation** — free code signing for OSS | €0 (we are GPL-3.0 OSS) | application + review, external/slow |
| EV cert + Microsoft Partner Center (attestation) | ~€300-400/yr | medium |

itsmikethetech's Virtual-Display-Driver is signed exactly this way (SignPath).
**Recommendation: apply for SignPath OSS signing early** — it's the long pole
(external approval); everything else can be developed locally under test-signing
in parallel.

## 1. Own virtual display (drop the Parsec dependency)

Today the sender drives the third-party **parsec-vdd** driver (installed
out-of-band, `nefconw` + registry dance). Replace it with a driver we own:

- **Fork the Microsoft IddCx sample / Virtual-Display-Driver** (MIT), rebrand,
  bundle it — do **not** write one from scratch; the sample is 90% of the work.
- Removes the Parsec runtime dependency *and* the external install step. The
  sender talks to our own adapter (own GUID); resolution is already set through
  the Win32 display APIs.
- **Stable monitor identity, for free.** parsec-vdd hands the monitor a fresh
  UID on every add (`DISPLAY\PSCCDD0\…&UID256`, `…&UID257`, …), so Windows can't
  remember its position and we persist/reapply it ourselves in HKCU. Our own
  driver can emit a fixed EDID/serial → Windows remembers the arrangement
  natively, retiring that workaround (and the ghost-device buildup below).
- Effort: **M** (driver code) + external (signing, see above).

## 2. Background / service model

**A naive Windows service will not work here.** Services run in session 0,
isolated — from there you can do **neither Desktop Duplication of the user's
session nor `SendInput`** into it. A "proper" service needs a split: a session-0
service (lifecycle) **plus** an interactive agent launched into the user session
via `WTSQueryUserToken` / `CreateProcessAsUser` (how Sunshine / Parsec / RDP do
it).

**Largely done.** It turned out the app doesn't need elevation at all — the only
admin-requiring step is registering an iPad resolution as a parsec-vdd custom
mode (a one-off HKLM write, which self-elevates once). So the tray app runs
un-elevated and **autostart is a plain per-user Run entry** (Settings → *Start
with Windows*), no scheduled task and no logon UAC. The full session-0
service + interactive agent split is only worth it if it must run *before* user
logon / fully headless — not needed for "a second monitor for my own PC".

## 3. Tray app + settings GUI — done

- Native Win32 tray app (`Shell_NotifyIcon`) that *is* the sender host: a
  status-dot icon, a Connect/Disconnect/Settings/Run-as-admin/Exit menu, and a
  settings dialog (iPad IP, port, auto-connect, Start-with-Windows).
- `SenderApp` is controllable (Start/Stop/state); config persists as JSON in
  `%APPDATA%`.
- Remaining polish: async Stop so the UI never briefly blocks (see known issues);
  a richer C#/WinUI front-end is possible later but not needed.

## 4. Installer / distribution

Depends on item 1 (a signed driver).

- Tooling: **Inno Setup** (simpler than WiX for "app + driver + auto-start
  task"). **MSIX is out** — it can't cleanly install drivers.
- Steps: lay down the app, `pnputil /add-driver /install` the signed driver,
  register the auto-start task, create shortcuts, and an uninstaller that also
  removes the virtual display + driver. Sign the installer with the same cert.

## Additional ideas

Some from the [#65](https://github.com/peetzweg/opendisplay/issues/65)
component-mapping, some ours:

- **mDNS/Bonjour discovery** — auto-find the iPad instead of typing an IP
  (Bonjour SDK for Windows). Big UX win; drops straight into the tray GUI as a
  device list.
- **USB transport** — the Apple Mobile Device Service *is* usbmuxd on Windows
  (ships with iTunes / Apple Devices) + libimobiledevice → TCP-over-USB. Lower
  latency, no WiFi, no IP entry.
- **Multiple iPads at once** — each iPad as its own virtual monitor + connection
  + capture/encode pipeline (today one running instance drives one iPad). Hard
  rule: **exactly one connection per iPad/IP** — a second connection to the same
  device must be refused (see the connection guard under Known issues), while
  *different* iPads may be driven simultaneously.
- **⚠️ Security** — the link is currently **unencrypted and unauthenticated**:
  anyone on the LAN can connect, see the screen, **and inject mouse input**
  (i.e. remote-control the PC). A real blocker for public distribution — add
  pairing (PIN) and optionally TLS *before* shipping widely.
- **Performance** — the BGRA→NV12 color conversion is CPU single-threaded and
  noticeable at panel resolutions like 2732×2048. Move it to the GPU and feed
  D3D11 textures zero-copy into the Media Foundation encoder → less CPU, lower
  latency, more resolution headroom.
- **Clock-sync / latency overlay** — the telemetry prefix + `pong` reply we
  currently skip would light up the iPad's latency overlay. Nice-to-have.
- **Smaller items** — bitrate/fps/resolution as GUI settings + adaptive bitrate;
  GPU device-lost / display-sleep / multi-GPU robustness; auto-update.

## Known issues & smaller open items

Most items found during the PoC are now addressed: per-iPad connection guard,
live-move input tracking, a bounded (~3 s) connect timeout so Disconnect and
reconnect stay snappy, silenced encoder log noise, a clean tray shutdown, and
`.gitattributes`. What's left:

- **Phantom virtual monitors accumulate ~1 per run.** parsec-vdd mints a fresh
  monitor UID on every add and, when the process dies, unplugs it but leaves the
  devnode behind. `opendisplay-win.exe --cleanup-monitors` removes the leftovers
  on demand. Deliberately **not** run automatically — device removal is too
  invasive for the runtime path — and the own driver (§1, stable identity)
  removes the root cause entirely (it wouldn't churn UIDs).
- **Keyframe cadence spikes under motion.** Heavy on-screen change emits far
  more IDR frames than the 2 s GOP implies (active-tail re-encode + NVENC
  scene-change I-frames) — bandwidth only, not correctness; folds into the
  adaptive-bitrate item above.

## Suggested order

1. **Apply for SignPath OSS signing** — the long pole; gates 1 and 4.
2. **Tray app + `SenderApp` refactor** (simplified 2 + 3) — needs no driver, can
   start immediately.
3. **Own IddCx driver** (1) — develop locally under test-signing while signing
   is pending.
4. **Installer** (4) — once the driver is signed.
5. **Security** (pairing / TLS) — before any public distribution.
