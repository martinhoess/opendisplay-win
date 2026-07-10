#pragma once

#include <string>

namespace od {

// Owns the whole sender lifecycle: connect, wait for hello, build the
// capture/encode pipeline, stream, and recover from disconnects/rotation.
// Run() blocks forever, reconnecting on any drop (spec M4).
class SenderApp {
public:
    explicit SenderApp(std::string ipadIp) : ip_(std::move(ipadIp)) {}

    void Run();

private:
    std::string ip_;
};

} // namespace od
