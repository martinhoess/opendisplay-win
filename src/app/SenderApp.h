#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace od {

class Connection;

// Owns the sender lifecycle: connect, wait for hello, build the capture/encode
// pipeline, stream, recover from disconnects/rotation. Can run either on a
// background thread (Start/Stop, for the tray GUI) or inline (RunBlocking, for
// the headless CLI). The reconnect loop runs until stopped.
class SenderApp {
public:
    enum class State { Idle, Connecting, Streaming };

    SenderApp() = default;
    ~SenderApp();

    SenderApp(const SenderApp&) = delete;
    SenderApp& operator=(const SenderApp&) = delete;

    // Start streaming to ip:port on a background thread. No-op if already
    // running (Stop first to retarget).
    void Start(std::string ip, uint16_t port);

    // Signal stop and join the worker. Safe to call when not running.
    void Stop();

    // Run the loop inline on the calling thread until the process is killed
    // (headless CLI). Does not return under normal operation.
    void RunBlocking(std::string ip, uint16_t port);

    bool IsRunning() const { return running_.load(); }
    State GetState() const { return state_.load(); }
    uint32_t Width() const { return width_.load(); }   // current capture px, 0 until connected
    uint32_t Height() const { return height_.load(); }

private:
    void RunLoop(std::string ip, uint16_t port);
    void InterruptibleSleep(int ms);

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<State> state_{State::Idle};
    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};

    // Lets Stop() close the socket the worker is currently blocked on
    // (Connect's result / ReadFrame), so a stop doesn't wait for the next
    // timeout. Guarded because Stop() runs on another thread.
    std::mutex connMutex_;
    Connection* activeConn_ = nullptr;
};

} // namespace od
