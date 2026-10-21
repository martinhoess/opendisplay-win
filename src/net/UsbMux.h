#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <winsock2.h>

namespace od {

struct UsbMuxDevice {
    bool present = false;
    uint32_t deviceId = 0;
    std::string serial;
};

// True when Apple Mobile Device Service (usbmuxd on 127.0.0.1:27015) has a USB
// iPad. End-user prerequisite is iTunes / Apple Devices; we speak usbmux ourselves.
bool ProbeUsbMux(UsbMuxDevice& out);

// Every USB-attached device usbmuxd knows (Wi-Fi "Network" entries skipped),
// in usbmuxd's order. False if usbmuxd could not be asked.
bool ListUsbMuxDevices(std::vector<UsbMuxDevice>& out);

// In-process localhost forwarder: 127.0.0.1:localPort -> usbmux -> device:devicePort.
// No external iproxy. Idempotent. Returns the local port on success.
bool EnsureUsbMuxForward(uint16_t devicePort, uint16_t localPort = 19000);

bool UsbMuxForwardListening(uint16_t localPort);

// Direct usbmux tunnel (no localhost hop). Socket is a regular TCP stream to
// the iPad receiver once the Connect Result is 0.
std::optional<SOCKET> ConnectUsbMux(uint32_t deviceId, uint16_t devicePort);

constexpr uint16_t kUsbMuxLocalPort = 19000;

// Attach/detach notifications from one shared usbmuxd "Listen" connection per
// process (started on first use). The generation goes up on every Attached /
// Detached message, when the listen connection opens or drops, and on
// WakeUsbMuxWaiters(). If usbmuxd is not reachable the watcher waits, without
// polling, until a later usbmux query succeeds (that query kicks it).
uint64_t UsbMuxEventGeneration();
// Blocks until the generation differs from `seen` or `deadline` passes.
// Returns the current generation.
uint64_t WaitUsbMuxEvent(uint64_t seen, std::chrono::steady_clock::time_point deadline);
// Wake every WaitUsbMuxEvent caller (e.g. its session is ending).
void WakeUsbMuxWaiters();

} // namespace od
