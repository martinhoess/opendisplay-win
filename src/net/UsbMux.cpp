#include "net/UsbMux.h"

#include "app/Log.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ws2tcpip.h>
#include <windows.h>

namespace od {
namespace {

constexpr uint16_t kUsbmuxPort = 27015;
constexpr DWORD kMuxTimeoutMs = 3000;

struct MuxHeader {
    uint32_t length;
    uint32_t version;
    uint32_t message;
    uint32_t tag;
};

bool SendAll(SOCKET s, const char* p, int n)
{
    while (n > 0) {
        int w = send(s, p, n, 0);
        if (w <= 0)
            return false;
        p += w;
        n -= w;
    }
    return true;
}

bool RecvAll(SOCKET s, char* p, int n)
{
    while (n > 0) {
        int r = recv(s, p, n, 0);
        if (r <= 0)
            return false;
        p += r;
        n -= r;
    }
    return true;
}

SOCKET ConnectLocal(uint16_t port, DWORD timeoutMs)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int cr = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    bool ok = (cr == 0);
    if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
        WSAPOLLFD pfd{};
        pfd.fd = s;
        pfd.events = POLLWRNORM;
        if (WSAPoll(&pfd, 1, static_cast<int>(timeoutMs)) > 0 && (pfd.revents & POLLWRNORM)) {
            int soErr = 0;
            int len = sizeof(soErr);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len) == 0 && soErr == 0)
                ok = true;
        }
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    if (!ok) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    return s;
}

bool SendPlist(SOCKET s, uint32_t tag, const std::string& xml)
{
    MuxHeader h{};
    h.length = static_cast<uint32_t>(16 + xml.size());
    h.version = 1;
    h.message = 8; // MESSAGE_PLIST
    h.tag = tag;
    if (!SendAll(s, reinterpret_cast<const char*>(&h), 16))
        return false;
    return SendAll(s, xml.data(), static_cast<int>(xml.size()));
}

bool RecvPlist(SOCKET s, std::string& xml)
{
    MuxHeader h{};
    if (!RecvAll(s, reinterpret_cast<char*>(&h), 16))
        return false;
    if (h.length < 16 || h.length > 1'000'000)
        return false;
    xml.resize(h.length - 16);
    if (h.length > 16 && !RecvAll(s, xml.data(), static_cast<int>(xml.size())))
        return false;
    return true;
}

bool ExtractKeyInteger(const std::string& xml, const char* key, uint32_t& value)
{
    std::string needle = std::string("<key>") + key + "</key>";
    auto pos = xml.find(needle);
    if (pos == std::string::npos)
        return false;
    auto i = xml.find("<integer>", pos);
    if (i == std::string::npos || i > pos + 80)
        return false;
    i += 9;
    auto e = xml.find("</integer>", i);
    if (e == std::string::npos)
        return false;
    try {
        value = static_cast<uint32_t>(std::stoul(xml.substr(i, e - i)));
        return true;
    } catch (...) {
        return false;
    }
}

bool ExtractKeyString(const std::string& xml, const char* key, std::string& value)
{
    std::string needle = std::string("<key>") + key + "</key>";
    auto pos = xml.find(needle);
    if (pos == std::string::npos)
        return false;
    auto i = xml.find("<string>", pos);
    if (i == std::string::npos || i > pos + 80)
        return false;
    i += 8;
    auto e = xml.find("</string>", i);
    if (e == std::string::npos)
        return false;
    value = xml.substr(i, e - i);
    return true;
}

std::string ConnectPlist(uint32_t deviceId, uint16_t devicePort)
{
    char buf[700];
    // usbmux PortNumber is network-endian stored as an integer.
    const uint16_t nport = htons(devicePort);
    snprintf(buf, sizeof(buf),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\"><dict>"
             "<key>ClientVersionString</key><string>opendisplay-win</string>"
             "<key>MessageType</key><string>Connect</string>"
             "<key>ProgName</key><string>opendisplay-win</string>"
             "<key>DeviceID</key><integer>%u</integer>"
             "<key>PortNumber</key><integer>%u</integer>"
             "</dict></plist>",
             deviceId, nport);
    return buf;
}

std::string ListDevicesPlist()
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\"><dict>"
           "<key>ClientVersionString</key><string>opendisplay-win</string>"
           "<key>MessageType</key><string>ListDevices</string>"
           "<key>ProgName</key><string>opendisplay-win</string>"
           "<key>kLibUSBMuxVersion</key><integer>3</integer>"
           "</dict></plist>";
}

void SpliceOneWay(SOCKET from, SOCKET to, std::atomic<bool>* stop)
{
    char buf[64 * 1024];
    while (!stop->load(std::memory_order_relaxed)) {
        int n = recv(from, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        if (!SendAll(to, buf, n))
            break;
    }
    shutdown(to, SD_SEND);
    stop->store(true, std::memory_order_relaxed);
}

struct Forwarder {
    std::mutex mu;
    SOCKET listenSock = INVALID_SOCKET;
    std::thread acceptThread;
    std::atomic<bool> running{false};
    uint16_t devicePort = 9000;
    uint16_t localPort = 19000;
    uint32_t deviceId = 0;
};

Forwarder g_fwd;

void AcceptLoop()
{
    while (g_fwd.running.load(std::memory_order_relaxed)) {
        SOCKET client = accept(g_fwd.listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!g_fwd.running.load(std::memory_order_relaxed))
                break;
            Sleep(50);
            continue;
        }
        BOOL noDelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

        UsbMuxDevice dev{};
        SOCKET tun = INVALID_SOCKET;
        if (ProbeUsbMux(dev) && dev.present) {
            auto opt = ConnectUsbMux(dev.deviceId, g_fwd.devicePort);
            if (opt)
                tun = *opt;
        }
        if (tun == INVALID_SOCKET) {
            Logf("usb", "usbmux forward: Connect failed, closing client\n");
            closesocket(client);
            continue;
        }

        auto stop = std::make_shared<std::atomic<bool>>(false);
        std::thread([client, tun, stop] {
            SpliceOneWay(client, tun, stop.get());
        }).detach();
        std::thread([client, tun, stop] {
            SpliceOneWay(tun, client, stop.get());
            closesocket(client);
            closesocket(tun);
        }).detach();
    }
}

} // namespace

bool ProbeUsbMux(UsbMuxDevice& out)
{
    out = {};
    SOCKET s = ConnectLocal(kUsbmuxPort, 400);
    if (s == INVALID_SOCKET)
        return false;
    bool ok = SendPlist(s, 1, ListDevicesPlist());
    std::string xml;
    if (ok)
        ok = RecvPlist(s, xml);
    closesocket(s);
    if (!ok)
        return false;

    // Prefer a USB attachment (skip Wi-Fi "Network" mux entries).
    auto usbPos = xml.find("<string>USB</string>");
    if (usbPos == std::string::npos)
        return false;
    // SerialNumber sits in the same Properties dict as ConnectionType.
    ExtractKeyString(xml, "SerialNumber", out.serial);
    if (!ExtractKeyInteger(xml, "DeviceID", out.deviceId))
        return false;
    out.present = true;
    return true;
}

std::optional<SOCKET> ConnectUsbMux(uint32_t deviceId, uint16_t devicePort)
{
    SOCKET s = ConnectLocal(kUsbmuxPort, kMuxTimeoutMs);
    if (s == INVALID_SOCKET)
        return std::nullopt;
    if (!SendPlist(s, 2, ConnectPlist(deviceId, devicePort))) {
        closesocket(s);
        return std::nullopt;
    }
    std::string xml;
    if (!RecvPlist(s, xml)) {
        closesocket(s);
        return std::nullopt;
    }
    uint32_t number = 0xFFFFFFFFu;
    ExtractKeyInteger(xml, "Number", number);
    if (number != 0) {
        Logf("usb", "usbmux Connect Result=%u (device %u port %u)\n", number, deviceId, devicePort);
        closesocket(s);
        return std::nullopt;
    }
    return s;
}

bool UsbMuxForwardListening(uint16_t localPort)
{
    SOCKET s = ConnectLocal(localPort, 200);
    if (s == INVALID_SOCKET)
        return false;
    closesocket(s);
    return true;
}

bool EnsureUsbMuxForward(uint16_t devicePort, uint16_t localPort)
{
    UsbMuxDevice dev{};
    if (!ProbeUsbMux(dev) || !dev.present)
        return false;

    std::lock_guard<std::mutex> lock(g_fwd.mu);
    g_fwd.devicePort = devicePort;
    g_fwd.deviceId = dev.deviceId;
    g_fwd.localPort = localPort;

    if (g_fwd.running.load(std::memory_order_relaxed) && g_fwd.listenSock != INVALID_SOCKET)
        return true;

    if (UsbMuxForwardListening(localPort)) {
        // Something (previous run / iproxy) already forwards this port.
        Logf("usb", "usbmux localhost :%u already listening - reusing\n", localPort);
        return true;
    }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET)
        return false;
    BOOL reuse = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(localPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(ls, 4) != 0) {
        Logf("usb", "usbmux bind 127.0.0.1:%u failed (%d)\n", localPort, WSAGetLastError());
        closesocket(ls);
        return false;
    }
    g_fwd.listenSock = ls;
    g_fwd.running = true;
    g_fwd.acceptThread = std::thread(AcceptLoop);
    g_fwd.acceptThread.detach();
    Logf("usb", "usbmux forward started: 127.0.0.1:%u -> USB device %s port %u\n",
         localPort, dev.serial.empty() ? "?" : dev.serial.c_str(), devicePort);
    return true;
}

} // namespace od
