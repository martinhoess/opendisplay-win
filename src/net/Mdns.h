#pragma once

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

// Runs the response parser against a well-formed packet and a handful of
// malformed ones (truncated names, a TXT length running past the record, a
// compression pointer loop, a header claiming records that aren't there).
// Prints what it checked and returns false on the first mismatch. Wired to
// `--browse-mdns`, because this parser is the only place in the sender that
// reads data from whoever happens to answer on the network.
bool SelfCheck();

} // namespace od
