#include "app/Config.h"

#include <windows.h>

#include <cctype>
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

// Minimal flat-JSON field lookups — the config only ever holds a handful of
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

} // namespace

std::wstring Config::FilePath()
{
    return AppDataDir() + L"\\config.json";
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

    cfg.ip = ReadString(json, "ip");
    long port = 0;
    if (ReadInt(json, "port", port) && port > 0 && port <= 65535)
        cfg.port = static_cast<uint16_t>(port);
    ReadBool(json, "autoReconnect", cfg.autoReconnect);

    return cfg;
}

void Config::Save() const
{
    std::wstring dir = AppDataDir();
    CreateDirectoryW(dir.c_str(), nullptr); // no-op if it already exists

    std::ofstream file(FilePath(), std::ios::binary | std::ios::trunc);
    if (!file)
        return;

    file << "{\n"
         << "  \"ip\": \"" << ip << "\",\n"
         << "  \"port\": " << port << ",\n"
         << "  \"autoReconnect\": " << (autoReconnect ? "true" : "false") << "\n"
         << "}\n";
}

} // namespace od
