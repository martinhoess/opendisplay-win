#include "net/CursorUdp.h"

#include <cstdio>
#include <cstring>

namespace od {

bool CursorUdp::Open(const std::string& host, uint16_t port)
{
    Close();

    // Match Connection::Connect: IPv4 only. AF_UNSPEC can pick an IPv6
    // route while TCP is on v4, so cursorAck never comes back.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || res == nullptr)
        return false;

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        std::memcpy(&dest_, p->ai_addr, p->ai_addrlen);
        destLen_ = static_cast<int>(p->ai_addrlen);
        break;
    }
    freeaddrinfo(res);

    if (s == INVALID_SOCKET)
        return false;

    // Non-blocking so a full send buffer never stalls the capture loop.
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_ = s;
    return true;
}

void CursorUdp::Close()
{
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    destLen_ = 0;
    std::memset(&dest_, 0, sizeof(dest_));
}

bool CursorUdp::Send(const uint8_t* data, size_t size)
{
    if (sock_ == INVALID_SOCKET || data == nullptr || size == 0 || destLen_ == 0)
        return false;
    const int n = ::sendto(sock_, reinterpret_cast<const char*>(data),
                           static_cast<int>(size), 0,
                           reinterpret_cast<const sockaddr*>(&dest_), destLen_);
    return n == static_cast<int>(size);
}

} // namespace od
