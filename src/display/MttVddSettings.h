#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace od {

// MTT Virtual Display Driver live config lives at
// C:\VirtualDisplayDriver\{vdd_settings.xml,user_edid.bin}.
// The mode list is generated per iPad from its hello size (MttModesForHello)
// and written as user_edid.bin (CustomEdid=true, fixed MTT/0x1337/"ODW1"
// identity so Windows keeps it the same monitor) plus mirrored XML
// <resolutions> (stock MTT still feeds IddCx QueryTargetModes from XML).
// Writing the live path + reloading MttVDD needs admin; the self-elevating
// --ensure-mtt-edid WxH one-shot covers that, and only runs when the bytes
// actually change. The shipped assets/mtt/user_edid.bin (11" default) is the
// fallback when no hello size is known.

struct MttMode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t hz = 60;
};

struct MttEnsureResult {
    bool ok = false;       // settings readable / modes present (or write+reload succeeded)
    bool changed = false;  // XML and/or EDID was rewritten (reload ran)
    int added = 0;         // number of <resolution> entries in the rewritten set
    std::wstring path;     // live settings path used
    std::string detail;    // short status for logging
};

// Prefer C:\VirtualDisplayDriver\vdd_settings.xml when present.
std::wstring FindLiveSettingsPath();

// THE single source of MTT modes. Landscape (long side first), 16-px floor
// aligned, @60 Hz, max 4 (EDID DTD slots): native hello size (preferred), the
// closest-aspect sibling iPad panel (aspect within 1%, width within 8%), 3/4
// and 1/2 of native. Invalid / >4095 sizes fall back to the 2360x1640 list.
// Mirrored by tools/gen_user_edid.py modes_for_size (byte-identical EDID).
std::vector<MttMode> MttModesForHello(uint32_t helloW, uint32_t helloH);

// EDID 1.3 (128 bytes) for `modes`: fixed MTT / 0x1337 / serial "ODW1",
// CVT-RB-ish DTD per mode (preferred first), physical size from the preferred
// mode (~265 ppi), unused slots = monitor name / dummy, checksum. Empty on
// invalid input.
std::vector<uint8_t> BuildMttEdid(const std::vector<MttMode>& modes);

// Shipped fallback list (= MttModesForHello(2360, 1640), the modes baked into
// assets/mtt/user_edid.bin).
std::vector<MttMode> BakedMttModes();

// Modes the driver currently offers (live XML <resolutions>, preferred first);
// BakedMttModes when unreadable. Used to validate a saved mode.
std::vector<MttMode> CurrentMttModes();

// Install/refresh C:\VirtualDisplayDriver\user_edid.bin from the shipped asset,
// set CustomEdid=true + PreventSpoof=true, mirror BakedMttModes into XML
// <resolutions>, reload MttVDD. Idempotent when already current.
MttEnsureResult EnsureCustomEdid();

// Same for the EDID generated from a hello size. dryRun: report whether
// anything differs (r.changed) without writing or reloading.
MttEnsureResult EnsureCustomEdidForHello(uint32_t helloW, uint32_t helloH, bool dryRun);

// Rewrite <resolutions> to the wanted set only (prune other aspects) without
// touching gpu/options/monitors. Prefer EnsureCustomEdid for the sender path.
// Needs admin to write the live path; returns ok=false with detail on denial.
MttEnsureResult EnsureResolutions(const std::vector<MttMode>& wanted);

// Elevated one-shot: runs `--ensure-mtt-edid WxH` (shipped file when 0x0).
bool SelfElevateEnsure(uint32_t helloW, uint32_t helloH);

// Sender entry, called before the MTT head attaches: generate the EDID for
// this hello size; if bytes / flags / XML modes differ from what is installed,
// show the one-time notice and self-elevate to write + reload MttVDD. No
// change -> no UAC, no reload. UAC declined / failure -> log, keep the
// installed EDID (shipped fallback if none), return false; never blocks the
// connection.
bool EnsureMttResolutionsForHello(uint32_t helloW, uint32_t helloH, const std::string& logTag);

// Optional UI hook (tray balloon) shown right before an EDID-change UAC prompt.
// Called on a sender thread; the callee must marshal to its UI thread.
void SetMttUserNotifier(std::function<void(const std::wstring&)> fn);

// Read <monitors><count> from live settings. Returns -1 if missing/unreadable.
int ReadMonitorCount();

// Write <monitors><count>N</count> and reload MttVDD. Idempotent when already N.
// Needs admin to write the live path.
MttEnsureResult SetMonitorCount(uint32_t count);

// Elevated one-shot used by --set-mtt-monitor-count.
bool SelfElevateSetMonitorCount(uint32_t count);

// Ensure at least one MTT head is attached (set count>=1 + reload if needed).
// Logs via Logf. Cancels any pending link-loss grace teardown.
// Position policy: the saved layout is applied once per attachment - after a
// fresh attach by us, or once when adopting a head that was already on the
// desktop (previous run / reboot). When the head is already attached and was
// positioned, this is a no-op for layout (ACCESS_LOST / rotate / resize
// rebuilds must never move the monitor back). Refuses after BeginMttShutdown.
bool EnsureMttVddAttached(const std::string& logTag);

// Drop MTT heads via CDS detach. Prefer RequestMttVddTeardown so link-loss
// flaps keep the head sticky across USB<->Wi-Fi.
bool TearDownMttVdd(const std::string& logTag);

// How long after link loss (socket drop / transport flap) we keep the MTT head
// attached waiting for a client hello. Cancelled by EnsureMttVddAttached.
constexpr int kMttLinkLossGraceMs = 6000;

enum class MttTeardownReason {
    UserInitiated, // tray Disconnect / Exit / Stop / app quit
    LinkLoss,      // socket drop, usbmux fail, transport switch, pipeline fail
};

// UserInitiated: cancel grace and TearDown now.
// LinkLoss: start/restart the grace timer; TearDown only if it expires with
// no intervening Ensure/Cancel (client did not return).
// graceMs <= 0 uses kMttLinkLossGraceMs.
void RequestMttVddTeardown(const std::string& logTag, MttTeardownReason reason, int graceMs = 0);

// Cancel a pending link-loss grace teardown (client returned).
void CancelPendingMttVddTeardown();

// Persist the MTT head's current CCD position/mode under
// %APPDATA%\opendisplay-win\mtt_display.json, keyed by MTT1337 identity (not
// \\.\DISPLAYn). Logs "mtt: saved position (x,y) WxH@hz".
// Only saves when the MTT head is attached AND active in the CCD topology with
// a valid, non-cloned source mode; otherwise (inactive / "Show only on 1",
// absent, mid-reconfigure) returns false and leaves the file untouched.
bool SaveMttDisplayTopology(const std::string& logTag);

// Same, but skipped within a short settle window after one of our own display
// applies (see NoteSelfDisplayChange) so our applies are not treated as user
// layout changes. Used by ACCESS_LOST, WM_DISPLAYCHANGE and the streaming poll.
bool SaveMttDisplayTopologyIfUserChange(const std::string& logTag);

// True when mtt_display.json has a usable width x height that the current
// iPad's EDID offers (hz optional; 0 -> caller picks a default). A mode saved
// for a different iPad size returns false so the caller uses the preferred
// mode. Does not touch the desktop.
bool QueryMttSavedMode(uint32_t& width, uint32_t& height, uint32_t& hz);

// After CDS attach: find the MTT head by identity and reapply saved x,y and
// mode (WxH@hz) when available. No-op when nothing saved yet. Logs
// "mtt: restored position (x,y)" / "mtt: restoring saved mode WxH@hz".
bool RestoreMttDisplayTopology(const std::string& logTag);

// Save-only observer (never restores): persists Display Settings drags /
// resolution picks while streaming so they survive the next reconnect / run.
void PollMttDisplayTopology(const std::string& logTag, POINT& lastObserved);

// Quit / session end: stop any later EnsureMttVddAttached from re-attaching.
void BeginMttShutdown();

// Quit / WM_ENDSESSION / normal process exit: cancel grace, save layout (if
// active), tear the MTT head down and persist the detached topology. Safe to
// call repeatedly; implies BeginMttShutdown.
bool ShutdownMttVddForExit(const std::string& logTag);

// Tray startup: if an MTT head is already on the desktop (left over from a
// previous run / reboot), adopt it - the first Ensure applies the saved layout
// once - and arm a grace teardown so it does not linger without an iPad.
void AdoptStaleMttHeadAtStartup(const std::string& logTag, int graceMs);

} // namespace od
