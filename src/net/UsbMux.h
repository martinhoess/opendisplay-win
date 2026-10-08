#pragma once

#include <cstdint>
#include <optional>
#include <string>

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

// In-process localhost forwarder: 127.0.0.1:localPort -> usbmux -> device:devicePort.
// No external iproxy. Idempotent. Returns the local port on success.
bool EnsureUsbMuxForward(uint16_t devicePort, uint16_t localPort = 19000);

bool UsbMuxForwardListening(uint16_t localPort);

// Direct usbmux tunnel (no localhost hop). Socket is a regular TCP stream to
// the iPad receiver once the Connect Result is 0.
std::optional<SOCKET> ConnectUsbMux(uint32_t deviceId, uint16_t devicePort);

constexpr uint16_t kUsbMuxLocalPort = 19000;

} // namespace od
