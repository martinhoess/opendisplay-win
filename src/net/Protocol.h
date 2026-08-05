#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// Control-message model for the OpenDisplay wire protocol (receiver -> sender).
// Deliberately not a general JSON library: the protocol only ever carries a
// handful of flat, unescaped, single-level objects (see PhoneReceiver.swift),
// so a tiny field-scanner is enough and keeps the dependency footprint at zero.
namespace od {

enum class ControlType { Hello, Touch, Scroll, Pencil, Proximity, Kf, Unknown };

enum class TouchPhase { Began, Moved, Ended, Cancelled, Unknown };

enum class PencilPhase { Hover, Down, Move, Up, Unknown };

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

// pi/2: pen standing perpendicular to the glass. Used as the neutral default
// whenever a message carries no altitude.
inline constexpr double kPencilAltitudeUpright = 1.5707963267948966;

// Apple Pencil, receiver protocol >= 3 only. The receiver sends these instead
// of `touch` for pen input, but *only* once we announced ourselves as protocol
// 3 or newer in the `welcome` reply — otherwise it silently falls back to
// `touch` and pressure never reaches us.
struct PencilMsg {
    PencilPhase phase = PencilPhase::Unknown;
    double x = 0.0;        // normalized [0,1], origin top-left (as for touch)
    double y = 0.0;
    double pressure = 0.0; // [0,1]
    double azimuth = 0.0;  // radians, UIKit convention
    double altitude = kPencilAltitudeUpright; // radians
    // `rotation` (barrel roll) is on the wire but always 0: it needs an Apple
    // Pencil Pro, which upstream has not wired up yet. Not parsed.
};

struct ProximityMsg {
    bool entering = false; // pen entered (true) or left (false) hover range
    double x = 0.0;
    double y = 0.0;
};

struct ControlMessage {
    ControlType type = ControlType::Unknown;
    HelloMsg hello;
    TouchMsg touch;
    ScrollMsg scroll;
    PencilMsg pencil;
    ProximityMsg proximity;
};

// Wire classification rule (spec §3 / PhoneReceiver.handleAnnexB):
// control JSON iff size < 32768 AND payload[0] == '{' AND payload contains no 0x00 byte.
bool IsControlPayload(const uint8_t* data, size_t size);

// Parses a payload that already passed IsControlPayload(). Unrecognized "type"
// values (e.g. "stats", "pong") parse successfully as ControlType::Unknown so
// callers can ignore them without treating them as errors.
std::optional<ControlMessage> ParseControlMessage(const uint8_t* data, size_t size);

} // namespace od
