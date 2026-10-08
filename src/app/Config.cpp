
#include "app/Config.h"

#include <windows.h>

#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace od {

namespace {

std::wstring AppDataDir()
{
    wchar_t* appData = nullptr;
    size_t len = 0;
    std::wstring base;
    if (_wdupenv_s(&appData, &len, L"APPDATA") == 0 && appData) {
        base = appData;
        free(appData);
    }
    if (base.empty())
        base = L".";
    return base + L"\\opendisplay-win";
}

// Minimal flat-JSON field lookups - the config only ever holds a handful of
// unnested string/number/bool values, so a full parser would be overkill.
std::string_view ValueAfter(std::string_view json, std::string_view key)
{
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos)
        return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos)
        return {};
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
        ++pos;
    return json.substr(pos);
}

std::string ReadString(std::string_view json, std::string_view key)
{
    std::string_view v = ValueAfter(json, key);
    if (v.empty() || v.front() != '"')
        return {};
    size_t end = v.find('"', 1);
    if (end == std::string_view::npos)
        return {};
    return std::string(v.substr(1, end - 1));
}

bool ReadInt(std::string_view json, std::string_view key, long& out)
{
    std::string_view v = ValueAfter(json, key);
    if (v.empty())
        return false;
    char* endp = nullptr;
    long parsed = std::strtol(std::string(v.substr(0, 16)).c_str(), &endp, 10);
    if (endp == nullptr)
        return false;
    out = parsed;
    return true;
}

// Reads a flat array of strings ("devices": ["a", "b"]). The only nesting the
// config has, so it stays a scan for quoted values up to the closing bracket
// rather than a reason to pull in a JSON library.
std::vector<std::string> ReadStringArray(std::string_view json, std::string_view key)
{
    std::vector<std::string> out;
    std::string_view v = ValueAfter(json, key);
    if (v.empty() || v.front() != '[')
        return out;

    size_t end = v.find(']');
    if (end == std::string_view::npos)
        return out;
    v = v.substr(1, end - 1);

    for (size_t pos = v.find('"'); pos != std::string_view::npos; pos = v.find('"', pos + 1)) {
        size_t close = v.find('"', pos + 1);
        if (close == std::string_view::npos)
            break;
        out.emplace_back(v.substr(pos + 1, close - pos - 1));
        pos = close;
    }
    return out;
}

bool ReadBool(std::string_view json, std::string_view key, bool& out)
{
    std::string_view v = ValueAfter(json, key);
    if (v.rfind("true", 0) == 0) {
        out = true;
        return true;
    }
    if (v.rfind("false", 0) == 0) {
        out = false;
        return true;
    }
    return false;
}

// Normalize mode/preset strings; unknown -> defaults that match today's behavior.
std::string NormalizeMode(const std::string& s)
{
    return (s == "fixed") ? "fixed" : "dynamic";
}

std::string NormalizePreset(const std::string& s)
{
    if (s == "balanced" || s == "quality")
        return s;
    return "speed";
}

std::string PlainJsonString(const std::string& device)
{
    std::string out;
    for (char c : device)
        if (c != '"' && c != '\\')
            out += c;
    return out;
}

void WriteStringArray(std::ofstream& file, const char* key, const std::vector<std::string>& values)
{
    file << "  \"" << key << "\": [";
    for (size_t i = 0; i < values.size(); ++i)
        file << (i == 0 ? "\"" : ", \"") << PlainJsonString(values[i]) << "\"";
    file << "]";
}


bool IsLoopbackHost(std::string_view host)
{
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

// First token of "<address>[ <nickname>]" — mirrors TrayApp SplitDevice address.
std::string DeviceAddressToken(const std::string& entry)
{
    size_t space = entry.find_first_of(" \t");
    if (space == std::string::npos)
        return entry;
    return entry.substr(0, space);
}

} // namespace

std::wstring Config::FilePath()
{
    return AppDataDir() + L"\\config.json";
}

void Config::SyncTransportsFromPicturePreset()
{
    if (separateUsbWifiPicture)
        return;
    picturePreset = NormalizePreset(picturePreset);
    usbEncodeMode = "dynamic";
    wifiEncodeMode = "dynamic";
    usbEncodePreset = picturePreset;
    wifiEncodePreset = picturePreset;
}

Config Config::Load()
{
    Config cfg;

    std::ifstream file(FilePath(), std::ios::binary);
    if (!file)
        return cfg;

    std::stringstream ss;
    ss << file.rdbuf();
    std::string json = ss.str();

    cfg.devices = ReadStringArray(json, "devices");
    if (cfg.devices.empty()) {
        // Config written by the single-iPad version.
        std::string legacy = ReadString(json, "ip");
        if (!legacy.empty())
            cfg.devices.push_back(legacy);
    }

    cfg.deviceIds = ReadStringArray(json, "deviceIds");
    // Align length with devices (pad with empty, or trim extras).
    cfg.deviceIds.resize(cfg.devices.size());

    cfg.usbLinkSerials = ReadStringArray(json, "usbLinkSerials");
    cfg.usbLinkIds = ReadStringArray(json, "usbLinkIds");
    const size_t linkN = (std::min)(cfg.usbLinkSerials.size(), cfg.usbLinkIds.size());
    cfg.usbLinkSerials.resize(linkN);
    cfg.usbLinkIds.resize(linkN);

    long port = 0;
    if (ReadInt(json, "port", port) && port > 0 && port <= 65535)
        cfg.port = static_cast<uint16_t>(port);
    ReadBool(json, "autoReconnect", cfg.autoReconnect);
    ReadBool(json, "usbWifiFailover", cfg.usbWifiFailover);
    ReadBool(json, "separateUsbWifiPicture", cfg.separateUsbWifiPicture);

    {
        std::string v = ReadString(json, "usbEncodeMode");
        if (!v.empty())
            cfg.usbEncodeMode = NormalizeMode(v);
        v = ReadString(json, "usbEncodePreset");
        if (!v.empty())
            cfg.usbEncodePreset = NormalizePreset(v);
        v = ReadString(json, "wifiEncodeMode");
        if (!v.empty())
            cfg.wifiEncodeMode = NormalizeMode(v);
        v = ReadString(json, "wifiEncodePreset");
        if (!v.empty())
            cfg.wifiEncodePreset = NormalizePreset(v);
        v = ReadString(json, "picturePreset");
        if (!v.empty()) {
            cfg.picturePreset = NormalizePreset(v);
        } else {
            // Migrate old configs: one control from USB preset; if USB and
            // Wi-Fi differed, keep separate advanced settings on.
            cfg.picturePreset = cfg.usbEncodePreset;
            if (cfg.usbEncodeMode != cfg.wifiEncodeMode || cfg.usbEncodePreset != cfg.wifiEncodePreset)
                cfg.separateUsbWifiPicture = true;
        }
    }

    cfg.SyncTransportsFromPicturePreset();
    if (cfg.PruneUsbOnlyDevices() > 0)
        cfg.Save(); // drop USB-only leftovers from disk so they do not reappear
    return cfg;
}

void Config::Save() const
{
    Config out = *this;
    out.SyncTransportsFromPicturePreset();
    out.deviceIds.resize(out.devices.size());

    std::wstring dir = AppDataDir();
    CreateDirectoryW(dir.c_str(), nullptr); // no-op if it already exists

    std::ofstream file(FilePath(), std::ios::binary | std::ios::trunc);
    if (!file)
        return;

    // The names are free text from the settings dialog, and both the writer
    // here and the reader above are hand-rolled: an unescaped quote in a name
    // would split the entry at the wrong place on the next load - the name
    // truncated, the rest read back as another device. Quotes and backslashes
    // are dropped rather than escaped, because nothing needs them in a name and
    // dropping keeps the reader as simple as it is.
    file << "{\n";
    WriteStringArray(file, "devices", out.devices);
    file << ",\n";
    WriteStringArray(file, "deviceIds", out.deviceIds);
    file << ",\n";
    WriteStringArray(file, "usbLinkSerials", out.usbLinkSerials);
    file << ",\n";
    WriteStringArray(file, "usbLinkIds", out.usbLinkIds);
    file << ",\n"
         << "  \"port\": " << out.port << ",\n"
         << "  \"autoReconnect\": " << (out.autoReconnect ? "true" : "false") << ",\n"
         << "  \"usbWifiFailover\": " << (out.usbWifiFailover ? "true" : "false") << ",\n"
         << "  \"picturePreset\": \"" << NormalizePreset(out.picturePreset) << "\",\n"
         << "  \"separateUsbWifiPicture\": " << (out.separateUsbWifiPicture ? "true" : "false") << ",\n"
         << "  \"usbEncodeMode\": \"" << NormalizeMode(out.usbEncodeMode) << "\",\n"
         << "  \"usbEncodePreset\": \"" << NormalizePreset(out.usbEncodePreset) << "\",\n"
         << "  \"wifiEncodeMode\": \"" << NormalizeMode(out.wifiEncodeMode) << "\",\n"
         << "  \"wifiEncodePreset\": \"" << NormalizePreset(out.wifiEncodePreset) << "\"\n"
         << "}\n";
}


size_t Config::PruneUsbOnlyDevices()
{
    deviceIds.resize(devices.size());
    std::vector<std::string> keptDevices;
    std::vector<std::string> keptIds;
    keptDevices.reserve(devices.size());
    keptIds.reserve(devices.size());
    size_t removed = 0;
    for (size_t i = 0; i < devices.size(); ++i) {
        if (IsLoopbackHost(DeviceAddressToken(devices[i]))) {
            ++removed;
            continue;
        }
        keptDevices.push_back(devices[i]);
        keptIds.push_back(deviceIds[i]);
    }
    devices = std::move(keptDevices);
    deviceIds = std::move(keptIds);
    return removed;
}

void Config::RememberUsbSerialId(const std::string& serial, const std::string& id)
{
    if (serial.empty() || id.empty())
        return;
    for (size_t i = 0; i < usbLinkSerials.size() && i < usbLinkIds.size(); ++i) {
        if (usbLinkSerials[i] == serial) {
            usbLinkIds[i] = id;
            return;
        }
    }
    usbLinkSerials.push_back(serial);
    usbLinkIds.push_back(id);
}

std::string Config::IdForUsbSerial(const std::string& serial) const
{
    if (serial.empty())
        return {};
    for (size_t i = 0; i < usbLinkSerials.size() && i < usbLinkIds.size(); ++i) {
        if (usbLinkSerials[i] == serial)
            return usbLinkIds[i];
    }
    return {};
}


} // namespace od
