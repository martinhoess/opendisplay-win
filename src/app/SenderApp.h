#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace od {

class Connection;

// Owns the sender lifecycle: connect, wait for hello, build the capture/encode
// pipeline, stream, recover from disconnects/rotation. Can run either on a
// background thread (Start/Stop, for the tray GUI) or inline (RunBlocking, for
// the headless CLI). The reconnect loop runs until stopped.
class SenderApp {
public:
    // Blocked: an iPad with a different panel size is streaming. parsec-vdd
    // puts one custom resolution on all of its virtual monitors (and MTT has
    // one head), so this one would only get a letterboxed picture - it waits
    // for the other to finish.
    // DriverMissing: the display driver chosen in Settings (or, on auto, any
    // supported one) is not installed; StatusDetail() says which.
    enum class State { Idle, Connecting, Streaming, Blocked, DriverMissing };

    // How the current (or last) session reached the iPad. None when idle.
    enum class Transport { None, Usb, Wifi };

    SenderApp() = default;
    ~SenderApp();

    SenderApp(const SenderApp&) = delete;
    SenderApp& operator=(const SenderApp&) = delete;

    // Start streaming to ip:port on a background thread. No-op if already
    // running (Stop first to retarget).
    // entryId: the iPad's Bonjour TXT id from Settings (may be empty). Used to
    // pick this iPad's USB cable when several are plugged in.
    // idFollowsHello: a typed-in address; the id of the iPad whose Wi-Fi hello
    // answers there becomes the entry's id (EntryId(), status is notified).
    void Start(std::string ip, uint16_t port, std::string entryId = {}, bool idFollowsHello = false);

    // The entry's id: as started, or as learned from a Wi-Fi hello.
    std::string EntryId() const
    {
        std::lock_guard<std::mutex> lock(idMutex_);
        return entryId_;
    }

    // Signal stop and join the worker. Safe to call when not running.
    void Stop();

    // Run the loop inline on the calling thread until the process is killed
    // (headless CLI). Does not return under normal operation.
    void RunBlocking(std::string ip, uint16_t port);

    // Every sender posts `msg` (wParam/lParam 0) to `hwnd` whenever anything
    // the tray shows changes: state, transport, wait reason, pause. Coalesced:
    // at most one is in flight until the tray calls StatusMessageHandled().
    static void SetStatusWindow(void* hwnd, unsigned msg); // HWND, UINT
    static void StatusMessageHandled();

    bool IsRunning() const { return running_.load(); }
    State GetState() const { return state_.load(); }
    Transport GetTransport() const { return transport_.load(); }
    uint32_t Width() const { return width_.load(); }   // current capture px, 0 until connected
    uint32_t Height() const { return height_.load(); }

    // While Blocked: the panel size currently holding the display, so the UI
    // can name what this iPad is waiting for. Zero otherwise.
    uint32_t BlockedByWidth() const { return blockedByWidth_.load(); }
    uint32_t BlockedByHeight() const { return blockedByHeight_.load(); }
    // While Blocked: the sender holding the display (identity only - compare,
    // never dereference) and whether it is MTT's one-iPad-at-a-time head.
    const void* BlockedByApp() const { return blockedByApp_.load(); }
    bool BlockedExclusive() const { return blockedExclusive_.load(); }

    // Human-readable reason for DriverMissing (empty otherwise).
    std::string StatusDetail() const
    {
        std::lock_guard<std::mutex> lock(detailMutex_);
        return statusDetail_;
    }

private:
    void RunLoop(std::string ip, uint16_t port);
    void InterruptibleSleep(int ms);
    void SetStatusDetail(std::string detail)
    {
        {
            std::lock_guard<std::mutex> lock(detailMutex_);
            if (statusDetail_ == detail)
                return;
            statusDetail_ = std::move(detail);
        }
        NotifyStatus();
    }
    void SetState(State s)
    {
        if (state_.exchange(s) != s)
            NotifyStatus();
    }
    void SetTransport(Transport t)
    {
        if (transport_.exchange(t) != t)
            NotifyStatus();
    }
    static void NotifyStatus();

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<State> state_{State::Idle};
    std::atomic<Transport> transport_{Transport::None};
    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};
    std::atomic<uint32_t> blockedByWidth_{0};
    std::atomic<const void*> blockedByApp_{nullptr};
    std::atomic<bool> blockedExclusive_{false};
    std::atomic<uint32_t> blockedByHeight_{0};
    mutable std::mutex idMutex_;
    std::string entryId_; // set in Start; the worker may learn it from a hello
    bool idFollowsHello_ = false; // set in Start before the worker runs
    mutable std::mutex detailMutex_;
    std::string statusDetail_;

    // Lets Stop() close the socket the worker is currently blocked on
    // (Connect's result / ReadFrame), so a stop doesn't wait for the next
    // timeout. Guarded because Stop() runs on another thread.
    std::mutex connMutex_;
    Connection* activeConn_ = nullptr;
};

} // namespace od
