#include "net/Protocol.h"

#include <cctype>
#include <cstdlib>
#include <string_view>

namespace od {

namespace {

std::string_view AsView(const uint8_t* data, size_t size)
{
    return std::string_view(reinterpret_cast<const char*>(data), size);
}

// Finds "key" : <value-start> in a flat JSON object and returns the offset of
// the first character of the value (after any whitespace). Returns npos if
// the key isn't present.
size_t FindValueStart(std::string_view json, std::string_view key)
{
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos)
        return std::string_view::npos;

    pos += needle.size();
    pos = json.find(':', pos);
    if (pos == std::string_view::npos)
        return std::string_view::npos;

    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
        ++pos;

    return pos;
}

std::optional<std::string> FindStringField(std::string_view json, std::string_view key)
{
    size_t start = FindValueStart(json, key);
    if (start == std::string_view::npos || start >= json.size() || json[start] != '"')
        return std::nullopt;

    size_t end = json.find('"', start + 1);
    if (end == std::string_view::npos)
        return std::nullopt;

    return std::string(json.substr(start + 1, end - start - 1));
}

std::optional<double> FindNumberField(std::string_view json, std::string_view key)
{
    size_t start = FindValueStart(json, key);
    if (start == std::string_view::npos)
        return std::nullopt;

    size_t end = start;
    while (end < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-' ||
            json[end] == '+' || json[end] == '.' || json[end] == 'e' || json[end] == 'E'))
        ++end;

    if (end == start)
        return std::nullopt;

    return std::strtod(std::string(json.substr(start, end - start)).c_str(), nullptr);
}

TouchPhase ParseTouchPhase(const std::string& phase)
{
    if (phase == "began") return TouchPhase::Began;
    if (phase == "moved") return TouchPhase::Moved;
    if (phase == "ended") return TouchPhase::Ended;
    if (phase == "cancelled") return TouchPhase::Cancelled;
    return TouchPhase::Unknown;
}

} // namespace

bool IsControlPayload(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0 || size >= 32768)
        return false;

    if (data[0] != '{')
        return false;

    for (size_t i = 0; i < size; ++i) {
        if (data[i] == 0x00)
            return false;
    }

    return true;
}

std::optional<ControlMessage> ParseControlMessage(const uint8_t* data, size_t size)
{
    std::string_view json = AsView(data, size);

    auto type = FindStringField(json, "type");
    if (!type)
        return std::nullopt;

    ControlMessage msg;

    if (*type == "hello") {
        msg.type = ControlType::Hello;
        msg.hello.pixelsWide = static_cast<int>(FindNumberField(json, "pixelsWide").value_or(0));
        msg.hello.pixelsHigh = static_cast<int>(FindNumberField(json, "pixelsHigh").value_or(0));
        msg.hello.scale = static_cast<int>(FindNumberField(json, "scale").value_or(1));
        msg.hello.device = FindStringField(json, "device").value_or("");
        msg.hello.id = FindStringField(json, "id").value_or("");
    } else if (*type == "touch") {
        msg.type = ControlType::Touch;
        msg.touch.phase = ParseTouchPhase(FindStringField(json, "phase").value_or(""));
        msg.touch.x = FindNumberField(json, "x").value_or(0.0);
        msg.touch.y = FindNumberField(json, "y").value_or(0.0);
    } else if (*type == "scroll") {
        msg.type = ControlType::Scroll;
        msg.scroll.dx = FindNumberField(json, "dx").value_or(0.0);
        msg.scroll.dy = FindNumberField(json, "dy").value_or(0.0);
    } else if (*type == "kf") {
        msg.type = ControlType::Kf;
    } else {
        msg.type = ControlType::Unknown; // e.g. "stats", "pong" -> ignored by caller
    }

    return msg;
}

} // namespace od
