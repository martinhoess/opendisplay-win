#pragma once

#include <winsock2.h>
#include <windows.h>

#include <string>
#include <vector>

namespace od {

// An OpenDisplay receiver as it advertises itself over Bonjour.
struct MdnsReceiver {
    std::string address;  // IPv4 from the A record
    std::string instance; // Bonjour instance name — the name set in the app's settings
    std::string host;     // SRV target, e.g. "iPad-Pro.local" (the iOS device name)
    std::string id;       // TXT id=, the same install UUID the `hello` carries
    uint16_t port = 0;    // SRV port
};

// Browses `_opensidecar._tcp.local` for up to timeoutMs and returns what
// answered. The receivers publish PTR, SRV, TXT and A in one packet, so a
// single query is enough — no follow-up resolve.
//
// The query goes out on *every* IPv4 interface on purpose: left to Windows,
// multicast follows the route metric, which on a machine with VMware or Hyper-V
// adapters means the query never reaches the network the iPads are on.
std::vector<MdnsReceiver> BrowseReceivers(int timeoutMs);

// Event-driven discovery without a query timer: joins 224.0.0.251:5353 on
// every IPv4 interface and hears the announcements receivers send when they
// start or join the network, plus everyone else's answers. Also watches the
// IPv4 address table (NotifyAddrChange) so a new network is noticed at once.
class MdnsListener {
public:
    enum class Event { Heard, Wake, NetworkChanged, Stop };

    MdnsListener();
    ~MdnsListener();
    MdnsListener(const MdnsListener&) = delete;
    MdnsListener& operator=(const MdnsListener&) = delete;

    // Blocks (no timeout) until `stop` or `wake` is signalled, the network
    // changes (the multicast socket is rejoined on the new interfaces), or a
    // packet naming a receiver arrives (Heard; `out` holds what it named).
    Event Wait(HANDLE stop, HANDLE wake, std::vector<MdnsReceiver>& out);

private:
    void Open();
    void Close();
    void ArmAddrChange();

    SOCKET sock_ = INVALID_SOCKET;
    WSAEVENT sockEvent_ = WSA_INVALID_EVENT;
    HANDLE addrEvent_ = nullptr;
    HANDLE addrHandle_ = nullptr;
    OVERLAPPED addrOv_{};
    bool addrArmed_ = false;
};

// Runs the response parser against a well-formed packet and a handful of
// malformed ones (truncated names, a TXT length running past the record, a
// compression pointer loop, a header claiming records that aren't there).
// Prints what it checked and returns false on the first mismatch. Wired to
// `--browse-mdns`, because this parser is the only place in the sender that
// reads data from whoever happens to answer on the network.
bool SelfCheck();

} // namespace od
