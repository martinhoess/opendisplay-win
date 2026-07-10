#include "net/Connection.h"

#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace od {

Connection::~Connection()
{
    Close();
}

Connection::Connection(Connection&& other) noexcept : socket_(other.socket_)
{
    other.socket_ = INVALID_SOCKET;
}

Connection& Connection::operator=(Connection&& other) noexcept
{
    if (this != &other) {
        Close();
        socket_ = other.socket_;
        other.socket_ = INVALID_SOCKET;
    }
    return *this;
}

std::optional<Connection> Connection::Connect(const std::string& ip, uint16_t port)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    if (getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &result) != 0)
        return std::nullopt;

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;

        if (::connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0)
            break;

        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);

    if (s == INVALID_SOCKET)
        return std::nullopt;

    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    return Connection(s);
}

bool Connection::WaitWritable(int timeoutMs)
{
    if (!IsValid())
        return false;

    WSAPOLLFD pfd{};
    pfd.fd = socket_;
    pfd.events = POLLWRNORM;
    int r = WSAPoll(&pfd, 1, timeoutMs);
    return r > 0 && (pfd.revents & POLLWRNORM) != 0;
}

bool Connection::ReadExact(uint8_t* buffer, size_t size)
{
    size_t total = 0;
    while (total < size) {
        int n = recv(socket_, reinterpret_cast<char*>(buffer + total), static_cast<int>(size - total), 0);
        if (n <= 0)
            return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

bool Connection::WriteExact(const uint8_t* buffer, size_t size)
{
    size_t total = 0;
    while (total < size) {
        int n = send(socket_, reinterpret_cast<const char*>(buffer + total), static_cast<int>(size - total), 0);
        if (n <= 0)
            return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

std::optional<std::vector<uint8_t>> Connection::ReadFrame()
{
    if (!IsValid())
        return std::nullopt;

    uint8_t lenBytes[4];
    if (!ReadExact(lenBytes, sizeof(lenBytes)))
        return std::nullopt;

    uint32_t len = (static_cast<uint32_t>(lenBytes[0]) << 24) |
                    (static_cast<uint32_t>(lenBytes[1]) << 16) |
                    (static_cast<uint32_t>(lenBytes[2]) << 8) |
                    static_cast<uint32_t>(lenBytes[3]);

    // The iPad only ever sends us small control JSON (hello/touch/scroll/kf,
    // all < 32 KB per spec §3). A length larger than this cap means a corrupt
    // or desynced stream — refuse it rather than attempt a multi-GB allocation
    // from an attacker/garbage-controlled 32-bit length.
    constexpr uint32_t kMaxInboundFrame = 1u << 20; // 1 MiB, generous
    if (len > kMaxInboundFrame)
        return std::nullopt;

    std::vector<uint8_t> payload(len);
    if (len > 0 && !ReadExact(payload.data(), len))
        return std::nullopt;

    return payload;
}

bool Connection::SendFrame(const uint8_t* data, uint32_t size)
{
    if (!IsValid())
        return false;

    uint8_t lenBytes[4] = {
        static_cast<uint8_t>((size >> 24) & 0xFF),
        static_cast<uint8_t>((size >> 16) & 0xFF),
        static_cast<uint8_t>((size >> 8) & 0xFF),
        static_cast<uint8_t>(size & 0xFF),
    };

    if (!WriteExact(lenBytes, sizeof(lenBytes)))
        return false;

    if (size > 0 && !WriteExact(data, size))
        return false;

    return true;
}

void Connection::Close()
{
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

} // namespace od
