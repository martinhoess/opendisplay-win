#pragma once

#include <cstdint>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace od {

// UDP side channel for cursor positions (PROTOCOL.md §6.3).
// WiFi/LAN only; caller must not open this on a USB/usbmux binding.
class CursorUdp {
public:
    CursorUdp() = default;
    ~CursorUdp() { Close(); }

    CursorUdp(const CursorUdp&) = delete;
    CursorUdp& operator=(const CursorUdp&) = delete;

    // Dial host:port over UDP. Returns false on failure (caller stays on TCP).
    bool Open(const std::string& host, uint16_t port);
    void Close();

    bool IsOpen() const { return sock_ != INVALID_SOCKET; }
    bool Ready() const { return IsOpen(); }

    // Fire-and-forget datagram (raw UTF-8 JSON, no length prefix).
    bool Send(const uint8_t* data, size_t size);

private:
    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_storage dest_{};
    int destLen_ = 0;
};

} // namespace od
