#include "net/Mdns.h"

#include <winsock2.h>

#include <ws2tcpip.h>

#include <iphlpapi.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>

#pragma comment(lib, "iphlpapi.lib")

namespace od {

namespace {

constexpr char kService[] = "_opensidecar._tcp.local";
constexpr char kMulticastGroup[] = "224.0.0.251";
constexpr uint16_t kMulticastPort = 5353;

enum : uint16_t { kTypeA = 1, kTypePtr = 12, kTypeTxt = 16, kTypeSrv = 33 };

void PutUint16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

// A DNS question for the service's PTR record. QCLASS carries the unicast-reply
// bit (0x8000): the answer then comes straight back to our port instead of only
// to the multicast group, which we don't subscribe to.
std::vector<uint8_t> BuildQuery()
{
    std::vector<uint8_t> packet;
    PutUint16(packet, 0);  // transaction id — mDNS ignores it
    PutUint16(packet, 0);  // flags: standard query
    PutUint16(packet, 1);  // one question
    PutUint16(packet, 0);
    PutUint16(packet, 0);
    PutUint16(packet, 0);

    std::string name = kService;
    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos)
            dot = name.size();
        packet.push_back(static_cast<uint8_t>(dot - start));
        packet.insert(packet.end(), name.begin() + start, name.begin() + dot);
        start = dot + 1;
    }
    packet.push_back(0);

    PutUint16(packet, kTypePtr);
    PutUint16(packet, 0x8001); // IN, unicast reply requested
    return packet;
}

// Reads a (possibly compressed) DNS name. `pos` moves past the name in the
// record stream; a compression pointer ends the walk there. Returns false on a
// malformed packet rather than reading past the buffer.
bool ReadName(const uint8_t* data, size_t size, size_t& pos, std::string& out)
{
    out.clear();
    size_t cursor = pos;
    size_t after = 0;
    int hops = 0;

    while (cursor < size) {
        uint8_t length = data[cursor];
        if (length == 0) {
            ++cursor;
            pos = after != 0 ? after : cursor;
            return true;
        }
        if ((length & 0xC0) == 0xC0) {
            if (cursor + 1 >= size || ++hops > 16) // a pointer loop must not hang us
                return false;
            size_t target = ((static_cast<size_t>(length & 0x3F) << 8) | data[cursor + 1]);
            if (after == 0)
                after = cursor + 2;
            cursor = target;
            continue;
        }
        if (cursor + 1 + length > size)
            return false;
        if (!out.empty())
            out += '.';
        out.append(reinterpret_cast<const char*>(data + cursor + 1), length);
        cursor += 1 + length;
    }
    return false;
}

// Collects the records of one response into `found`, keyed by instance name.
void ParseResponse(const uint8_t* data, size_t size, std::map<std::string, MdnsReceiver>& found)
{
    if (size < 12)
        return;

    auto readUint16 = [&](size_t at) -> uint16_t {
        return static_cast<uint16_t>((data[at] << 8) | data[at + 1]);
    };

    uint16_t questions = readUint16(4);
    size_t records = readUint16(6) + readUint16(8) + readUint16(10);

    size_t pos = 12;
    std::string name;
    for (uint16_t i = 0; i < questions; ++i) {
        if (!ReadName(data, size, pos, name) || pos + 4 > size)
            return;
        pos += 4;
    }

    // SRV and A live on the host name, PTR/TXT on the instance name, so the
    // host records are matched up after the loop.
    std::map<std::string, std::string> addressByHost;

    for (size_t i = 0; i < records; ++i) {
        if (!ReadName(data, size, pos, name) || pos + 10 > size)
            return;
        uint16_t type = readUint16(pos);
        uint16_t dataLength = readUint16(pos + 8);
        pos += 10;
        if (pos + dataLength > size)
            return;

        size_t recordStart = pos;
        switch (type) {
            case kTypePtr: {
                std::string instance;
                size_t cursor = recordStart;
                if (ReadName(data, size, cursor, instance))
                    found[instance].instance = instance.substr(0, instance.find('.'));
                break;
            }
            case kTypeSrv: {
                if (dataLength < 7)
                    break;
                MdnsReceiver& receiver = found[name];
                receiver.instance = name.substr(0, name.find('.'));
                receiver.port = readUint16(recordStart + 4);
                std::string host;
                size_t cursor = recordStart + 6;
                if (ReadName(data, size, cursor, host))
                    receiver.host = host;
                break;
            }
            case kTypeTxt: {
                MdnsReceiver& receiver = found[name];
                receiver.instance = name.substr(0, name.find('.'));
                size_t cursor = recordStart;
                while (cursor < recordStart + dataLength) {
                    uint8_t length = data[cursor];
                    // A length that runs past the record ends the record, it
                    // does not license a read past it: this data comes from
                    // whoever answered on the network.
                    if (cursor + 1 + length > recordStart + dataLength)
                        break;
                    std::string entry(reinterpret_cast<const char*>(data + cursor + 1), length);
                    if (entry.rfind("id=", 0) == 0)
                        receiver.id = entry.substr(3);
                    cursor += 1 + length;
                }
                break;
            }
            case kTypeA: {
                if (dataLength != 4)
                    break;
                char text[INET_ADDRSTRLEN] = {};
                in_addr addr{};
                std::memcpy(&addr, data + recordStart, 4);
                inet_ntop(AF_INET, &addr, text, sizeof(text));
                addressByHost[name] = text;
                break;
            }
            default:
                break;
        }
        pos += dataLength;
    }

    for (auto& entry : found) {
        auto address = addressByHost.find(entry.second.host);
        if (address != addressByHost.end())
            entry.second.address = address->second;
    }
}

// Every IPv4 address this machine has, minus loopback — one query goes out per
// interface (see the header for why Windows' own choice is not enough).
std::vector<in_addr> LocalInterfaces()
{
    std::vector<in_addr> addresses;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    // Every other outcome (ERROR_NO_DATA when no adapter has IPv4 up, and the
    // rest) leaves the buffer untouched — walking it would follow whatever was
    // on the stack.
    if (result != NO_ERROR)
        return addresses;

    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            addresses.push_back(reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr);
        }
    }
    return addresses;
}

} // namespace

namespace {

// --- Self-check helpers -------------------------------------------------------
// Builds responses by hand so the parser can be fed malformed ones on purpose.

void AppendName(std::vector<uint8_t>& out, const std::string& name)
{
    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos)
            dot = name.size();
        out.push_back(static_cast<uint8_t>(dot - start));
        out.insert(out.end(), name.begin() + start, name.begin() + dot);
        start = dot + 1;
    }
    out.push_back(0);
}

void AppendRecord(std::vector<uint8_t>& out, const std::string& name, uint16_t type,
                  const std::vector<uint8_t>& payload)
{
    AppendName(out, name);
    PutUint16(out, type);
    PutUint16(out, 1);   // class IN
    PutUint16(out, 0);   // TTL high
    PutUint16(out, 120); // TTL low
    PutUint16(out, static_cast<uint16_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> ResponseHeader(uint16_t answers)
{
    std::vector<uint8_t> packet;
    PutUint16(packet, 0);
    PutUint16(packet, 0x8400); // response, authoritative
    PutUint16(packet, 0);      // no questions echoed
    PutUint16(packet, answers);
    PutUint16(packet, 0);
    PutUint16(packet, 0);
    return packet;
}

// One receiver, exactly as an iPad answers: PTR, SRV, TXT and A in one packet.
std::vector<uint8_t> GoodResponse()
{
    const std::string instance = "iPad._opensidecar._tcp.local";
    const std::string host = "iPad-Pro.local";

    std::vector<uint8_t> packet = ResponseHeader(4);

    std::vector<uint8_t> ptr;
    AppendName(ptr, instance);
    AppendRecord(packet, kService, kTypePtr, ptr);

    std::vector<uint8_t> srv;
    PutUint16(srv, 0); // priority
    PutUint16(srv, 0); // weight
    PutUint16(srv, 9000);
    AppendName(srv, host);
    AppendRecord(packet, instance, kTypeSrv, srv);

    std::vector<uint8_t> txt;
    const std::string id = "id=5C2B9E14-0000-4A00-B000-1234567890AB";
    txt.push_back(static_cast<uint8_t>(id.size()));
    txt.insert(txt.end(), id.begin(), id.end());
    AppendRecord(packet, instance, kTypeTxt, txt);

    AppendRecord(packet, host, kTypeA, {192, 0, 2, 42}); // TEST-NET-1, RFC 5737
    return packet;
}

} // namespace

bool SelfCheck()
{
    auto fail = [](const char* what) {
        printf("mdns self-check FAILED: %s\n", what);
        return false;
    };

    // The packet a receiver really sends must come back complete.
    std::map<std::string, MdnsReceiver> found;
    std::vector<uint8_t> good = GoodResponse();
    ParseResponse(good.data(), good.size(), found);
    if (found.size() != 1)
        return fail("a well-formed response should yield exactly one receiver");

    const MdnsReceiver& receiver = found.begin()->second;
    if (receiver.instance != "iPad" || receiver.host != "iPad-Pro.local" || receiver.port != 9000 ||
        receiver.address != "192.0.2.42" || receiver.id != "5C2B9E14-0000-4A00-B000-1234567890AB")
        return fail("a well-formed response was parsed into the wrong fields");

    // A TXT string claiming more bytes than the record holds must not be read
    // past — this is the one an attacker on the LAN would send.
    std::vector<uint8_t> overlongTxt = ResponseHeader(1);
    AppendRecord(overlongTxt, "iPad._opensidecar._tcp.local", kTypeTxt, {200, 'i', 'd', '=', 'x'});
    found.clear();
    ParseResponse(overlongTxt.data(), overlongTxt.size(), found);
    if (!found["iPad._opensidecar._tcp.local"].id.empty())
        return fail("an overlong TXT length was read past the record");

    // Truncations at every offset must be survivable — no read past the buffer,
    // no hang. Nothing is asserted about the result: a cut packet may legally
    // yield a partial receiver.
    for (size_t cut = 1; cut < good.size(); ++cut) {
        found.clear();
        ParseResponse(good.data(), cut, found);
    }

    // A compression pointer that points at itself must not loop forever.
    std::vector<uint8_t> pointerLoop = ResponseHeader(1);
    size_t nameAt = pointerLoop.size();
    pointerLoop.push_back(0xC0);
    pointerLoop.push_back(static_cast<uint8_t>(nameAt)); // points at itself
    PutUint16(pointerLoop, kTypeTxt);
    PutUint16(pointerLoop, 1);
    PutUint16(pointerLoop, 0);
    PutUint16(pointerLoop, 120);
    PutUint16(pointerLoop, 0);
    found.clear();
    ParseResponse(pointerLoop.data(), pointerLoop.size(), found);

    // A header promising records the packet doesn't contain must end the walk.
    std::vector<uint8_t> lyingHeader = ResponseHeader(40);
    AppendRecord(lyingHeader, "iPad._opensidecar._tcp.local", kTypeA, {10, 0, 0, 1});
    found.clear();
    ParseResponse(lyingHeader.data(), lyingHeader.size(), found);

    printf("mdns self-check ok: well-formed, overlong TXT, %zu truncations, pointer loop, lying header\n",
           good.size() - 1);
    return true;
}

std::vector<MdnsReceiver> BrowseReceivers(int timeoutMs)
{
    std::vector<uint8_t> query = BuildQuery();

    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(kMulticastPort);
    inet_pton(AF_INET, kMulticastGroup, &group.sin_addr);

    std::vector<SOCKET> sockets;
    for (const in_addr& local : LocalInterfaces()) {
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET)
            continue;

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr = local;
        DWORD ttl = 255;
        if (bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0 ||
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&local), sizeof(local)) != 0) {
            closesocket(sock);
            continue;
        }
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
        sendto(sock, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()), 0,
               reinterpret_cast<sockaddr*>(&group), sizeof(group));
        sockets.push_back(sock);
    }

    std::map<std::string, MdnsReceiver> found;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (!sockets.empty()) {
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
            break;

        fd_set readable;
        FD_ZERO(&readable);
        for (SOCKET sock : sockets)
            FD_SET(sock, &readable);

        timeval wait{};
        wait.tv_sec = static_cast<long>(remaining.count() / 1'000'000);
        wait.tv_usec = static_cast<long>(remaining.count() % 1'000'000);
        if (select(0, &readable, nullptr, nullptr, &wait) <= 0)
            break;

        uint8_t packet[9000];
        for (SOCKET sock : sockets) {
            if (!FD_ISSET(sock, &readable))
                continue;
            int length = recv(sock, reinterpret_cast<char*>(packet), sizeof(packet), 0);
            if (length > 0)
                ParseResponse(packet, static_cast<size_t>(length), found);
        }
    }

    for (SOCKET sock : sockets)
        closesocket(sock);

    std::vector<MdnsReceiver> receivers;
    for (auto& entry : found) {
        // A PTR alone (no SRV/A) says a name exists but not where — useless here.
        if (!entry.second.address.empty())
            receivers.push_back(entry.second);
    }
    return receivers;
}

} // namespace od
