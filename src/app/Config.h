#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace od {

// Encode mode / preset strings match config.json flat keys (usbEncodeMode, ...).
enum class EncodeMode { Dynamic, Fixed };
enum class EncodePreset { Speed, Balanced, Quality };

struct Config {
    // iPad addresses, in menu order; one sender (and one virtual monitor) per
    // entry. Empty until the user configures a device. A config from the
    // single-iPad version (a bare "ip") loads as a one-entry list.
    // Each entry is "<address>[ <nickname>]".
    std::vector<std::string> devices;
    // Parallel to devices: mDNS TXT id= when known (empty if added by hand).
    // Lets us re-find an iPad after its Wi-Fi address changes.
    std::vector<std::string> deviceIds;

    // Learned USB serial <-> hello.id bindings (parallel arrays). USB serial is
    // known at plug; TXT id arrives in hello. Used to merge Nearby rows when
    // more than one Wi-Fi device is visible (never guess without this).
    std::vector<std::string> usbLinkSerials;
    std::vector<std::string> usbLinkIds;
    uint16_t port = 9000;         // the iPad receiver's fixed listen port
    bool autoReconnect = true;    // start streaming automatically on launch

    // When USB is in use and the cable drops (or USB dial fails), fall back to
    // the configured Wi-Fi address. Default ON matches today's behavior.
    bool usbWifiFailover = true;

    // Simple Picture control: "speed" | "balanced" | "quality".
    // Always Dynamic under the hood when separateUsbWifiPicture is false.
    std::string picturePreset = "speed";

    // Off (default): one picturePreset drives both USB and Wi-Fi (Dynamic).
    // On: use the per-transport mode/preset fields below.
    bool separateUsbWifiPicture = false;

    // Per-transport encode prefs (flat JSON). Used when separateUsbWifiPicture
    // is on; otherwise kept in sync with picturePreset + Dynamic on Save.
    std::string usbEncodeMode = "dynamic";     // "dynamic" | "fixed"
    std::string usbEncodePreset = "speed";     // "speed" | "balanced" | "quality"
    std::string wifiEncodeMode = "dynamic";
    std::string wifiEncodePreset = "speed";

    // Loads the saved config; returns defaults if the file is missing/invalid.
    static Config Load();

    // Writes the config (creating the directory if needed). Best-effort.
    void Save() const;

    // When separateUsbWifiPicture is false, copy picturePreset into both
    // transports as Dynamic. No-op when separate is on.
    void SyncTransportsFromPicturePreset();

    // Record that this Apple USB serial presented hello.id (USB session).
    void RememberUsbSerialId(const std::string& serial, const std::string& id);
    // Empty if we have not seen hello for this serial yet.
    std::string IdForUsbSerial(const std::string& serial) const;

    // Drop leftover USB-discovery rows (127.0.0.1 / ::1 / localhost as the
    // primary Your iPads address). Real Nearby (Wi-Fi) adds keep a LAN IP;
    // USB is only a transport for those. Returns how many entries removed.
    size_t PruneUsbOnlyDevices();

    // Full path to the config file (also used to derive the app data dir).
    static std::wstring FilePath();
};

inline EncodeMode ParseEncodeMode(const std::string& s)
{
    return (s == "fixed") ? EncodeMode::Fixed : EncodeMode::Dynamic;
}

inline EncodePreset ParseEncodePreset(const std::string& s)
{
    if (s == "balanced")
        return EncodePreset::Balanced;
    if (s == "quality")
        return EncodePreset::Quality;
    return EncodePreset::Speed;
}

inline const char* EncodeModeName(EncodeMode m)
{
    return m == EncodeMode::Fixed ? "fixed" : "dynamic";
}

inline const char* EncodePresetName(EncodePreset p)
{
    switch (p) {
        case EncodePreset::Balanced: return "balanced";
        case EncodePreset::Quality: return "quality";
        default: return "speed";
    }
}

} // namespace od
