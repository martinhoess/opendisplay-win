#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace od {

// Which virtual display driver a sender streams from.
//   MTT    - MTT Virtual Display Driver (one sticky head, per-iPad EDID,
//            topology save/restore; see MttVddSettings.h).
//   Parsec - parsec-vdd (upstream's original backend, VirtualDisplay.h):
//            one virtual monitor per iPad, several iPads at once.
enum class DisplayBackendKind { Mtt, Parsec };

const char* DisplayBackendName(DisplayBackendKind kind); // "MTT VDD" / "Parsec VDD"

bool IsMttDriverInstalled();
bool IsParsecDriverInstalled();

// Config value "auto" | "mtt" | "parsec" (anything else = auto).
// auto: MTT when its driver is installed, otherwise Parsec. An explicit choice
// is never silently swapped: if that driver is missing, driverInstalled is
// false and `problem` says so (shown in the tray), and nothing connects.
struct DisplayBackendSelection {
    DisplayBackendKind kind = DisplayBackendKind::Mtt;
    bool automatic = true;
    bool driverInstalled = false;
    std::string problem; // empty when driverInstalled
};
DisplayBackendSelection SelectDisplayBackend(const std::string& configValue);

// Why the sender (re)builds its pipeline: a new/changed hello (may change the
// head's size) or a capture-side rebuild (ACCESS_LOST, resize/rotate seen in
// the capture) that must not reshape the head behind the user's back.
enum class BuildReason { Hello, CaptureRebuild };

// The part of the sender that differs between drivers: bring up the head,
// name it for capture, report where it is for input/cursor mapping, and the
// session hooks. Capture, encode and transport stay in SenderApp.
class DisplayBackend {
public:
    virtual ~DisplayBackend() = default;

    virtual DisplayBackendKind Kind() const = 0;

    // New connection: raw hello pixelsWide/High (unaligned).
    virtual void BeginConnection(uint32_t helloW, uint32_t helloH) = 0;

    // Make the head exist at width x height and be ready to capture. Caller
    // holds the pipeline lock and has already closed the duplication.
    virtual bool Prepare(uint32_t width, uint32_t height, BuildReason reason) = 0;

    // GDI device name of the head (\\.\DISPLAYn) for DesktopDuplication.
    virtual std::wstring DeviceName() const = 0;

    // Head rect on the virtual desktop. Thread-safe (reader / cursor threads).
    virtual RECT CurrentRect() = 0;

    // Current refresh of the head (cursor thread cadence); 60 if unknown.
    uint32_t RefreshHz() const;

    // Windows rotated the head: MTT forces it back to the hello landscape,
    // Parsec adopts the rotation (upstream behaviour).
    virtual bool KeepsHelloLandscape() const = 0;
    virtual bool ForceMode(uint32_t width, uint32_t height, uint32_t hz) = 0;

    // Head left on a foreign size: Parsec rebuilds once (another sender
    // changed the shared custom mode - upstream), MTT only logs.
    virtual bool RebuildsOnForeignSize() const = 0;

    virtual void OnStreaming() {}              // pipeline up, session streaming
    virtual void OnLayoutMayHaveChanged() {}   // before a capture-driven rebuild
    virtual void Poll() {}                     // capture loop, every ~2 s
    // lastClient: this was the last iPad on the head. userInitiated: tray
    // Disconnect / Quit (vs. link loss -> grace).
    virtual void OnSessionEnd(bool lastClient, bool userInitiated) { (void)lastClient; (void)userInitiated; }
    virtual void OnPipelineFailed(bool lastClient) { (void)lastClient; }
};

// logTag: the iPad address (log tag, Parsec saved-position key). cancel: the
// sender's stop flag (aborts a pending UAC wait on the MTT path).
std::unique_ptr<DisplayBackend> MakeDisplayBackend(DisplayBackendKind kind, const std::string& logTag, uint32_t fps,
                                                   const std::atomic<bool>* cancel);

} // namespace od
