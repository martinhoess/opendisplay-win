#pragma once

#include <cstdint>
#include <string>

namespace od {

// User settings, persisted as JSON at %APPDATA%\opendisplay-win\config.json.
// Flat and tiny — hand-rolled JSON, no dependency.
struct Config {
    std::string ip;               // iPad address; empty until the user sets one
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
