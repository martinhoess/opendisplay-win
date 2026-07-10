#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// Control-message model for the OpenDisplay wire protocol (receiver -> sender).
// Deliberately not a general JSON library: the protocol only ever carries five
// flat, unescaped, single-level objects (see PhoneReceiver.swift), so a tiny
// field-scanner is enough and keeps the dependency footprint at zero.
namespace od {

enum class ControlType { Hello, Touch, Scroll, Kf, Unknown };

enum class TouchPhase { Began, Moved, Ended, Cancelled, Unknown };

struct HelloMsg {
    int pixelsWide = 0;
    int pixelsHigh = 0;
    int scale = 1;
    std::string device;
    std::string id;
};

struct TouchMsg {
    TouchPhase phase = TouchPhase::Unknown;
    double x = 0.0; // normalized [0,1], origin top-left
    double y = 0.0;
};

struct ScrollMsg {
    double dx = 0.0; // video pixels
    double dy = 0.0;
};

struct ControlMessage {
    ControlType type = ControlType::Unknown;
    HelloMsg hello;
    TouchMsg touch;
    ScrollMsg scroll;
};

// Wire classification rule (spec §3 / PhoneReceiver.handleAnnexB):
// control JSON iff size < 32768 AND payload[0] == '{' AND payload contains no 0x00 byte.
bool IsControlPayload(const uint8_t* data, size_t size);

// Parses a payload that already passed IsControlPayload(). Unrecognized "type"
// values (e.g. "stats", "pong") parse successfully as ControlType::Unknown so
// callers can ignore them without treating them as errors.
std::optional<ControlMessage> ParseControlMessage(const uint8_t* data, size_t size);

} // namespace od
