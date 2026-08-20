#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace od {

// User settings, persisted as JSON at %APPDATA%\opendisplay-win\config.json.
// Flat and tiny — hand-rolled JSON, no dependency.
struct Config {
    // iPad addresses, in menu order; one sender (and one virtual monitor) per
    // entry. Empty until the user configures a device. A config from the
    // single-iPad version (a bare "ip") loads as a one-entry list.
    std::vector<std::string> devices;
    uint16_t port = 9000;         // the iPad receiver's fixed listen port
    bool autoReconnect = true;    // start streaming automatically on launch

    // Loads the saved config; returns defaults if the file is missing/invalid.
    static Config Load();

    // Writes the config (creating the directory if needed). Best-effort.
    void Save() const;

    // Full path to the config file (also used to derive the app data dir).
    static std::wstring FilePath();
};

} // namespace od
