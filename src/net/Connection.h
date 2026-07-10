#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Prevent windows.h from pulling in winsock.h before winsock2.h; caller must
// only ever include this header, never <windows.h> first, in TUs that use it.
#include <winsock2.h>

namespace od {

// One TCP connection to the iPad receiver at <ip>:9000, with the wire framing
// ([4-byte big-endian length][payload], identical in both directions) built in.
// We are the connecting side; the iPad listens (spec §1).
class Connection {
public:
    Connection() = default;
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    // Connects to ip:port, sets TCP_NODELAY. Returns nullopt on failure.
    static std::optional<Connection> Connect(const std::string& ip, uint16_t port);

    // Blocking read of the next framed message. Returns nullopt on
    // disconnect/socket error.
    std::optional<std::vector<uint8_t>> ReadFrame();

    // Sends payload as one framed message (length prefix + payload written as
    // a single send() call per short write loop). Returns false on error.
    // Sends the whole frame or fails — never a partial write on the wire
    // (sends are blocking, so a real error is the only way this stops early),
    // which is what keeps the receiver's length-prefixed framing in sync.
    bool SendFrame(const uint8_t* data, uint32_t size);

    // Returns true if the socket can accept data now (send buffer has room).
    // Used for backpressure: the caller drops a whole frame rather than
    // blocking on a congested link and building latency. timeoutMs 0 = poll.
    bool WaitWritable(int timeoutMs);

    bool IsValid() const { return socket_ != INVALID_SOCKET; }

    void Close();

private:
    explicit Connection(SOCKET s) : socket_(s) {}

    bool ReadExact(uint8_t* buffer, size_t size);
    bool WriteExact(const uint8_t* buffer, size_t size);

    SOCKET socket_ = INVALID_SOCKET;
};

} // namespace od
