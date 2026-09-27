// Logitech mice's own lighting effects over HID++ 2.0, as G HUB sends them (captured from a
// G502 X Plus through its LIGHTSPEED receiver, see docs/peripherals.md). LumaBridge uses them
// for the effects the mouse can run itself (breathing, color cycle, color wave): smooth and
// across every LED, where G HUB's LED SDK gives apps only one color. Pure packet building,
// tested.
//
//   11 <device> <feature index> <function << 4 | software ID> <parameters...>   (20 bytes)
//
// RGB effects (feature 0x8071) function 1, SetRgbClusterEffect:
//   <cluster> <effect index> <10 effect parameters> 01
// The effect index is the effect's place in the cluster's list (GetInfo, function 0), not
// its ID. With a last byte of 00 the mouse answers but doesn't change.
#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>

#include "color.h"
#include "effects.h"

namespace luma::app::hidpp {

constexpr uint16_t kVendor = 0x046D;
constexpr uint8_t kLong = 0x11;
constexpr uint8_t kSwId = 0x0A;  // LumaBridge's software ID (G HUB uses 0x0B)
constexpr uint8_t kWired = 0xFF; // device index of a mouse on its cable; 1-6 behind a receiver

constexpr uint16_t kFeatureName = 0x0005;
constexpr uint16_t kRgbEffects = 0x8071;
// Effect IDs in GetInfo's answers.
constexpr uint16_t kIdFixed = 0x0001, kIdCycle = 0x0003, kIdBreathing = 0x000A;

using Report = std::array<uint8_t, 20>;

inline Report Request(uint8_t device, uint8_t feature, uint8_t function, std::initializer_list<uint8_t> params) {
    Report r{kLong, device, feature, static_cast<uint8_t>(function << 4 | kSwId)};
    size_t i = 4;
    for (uint8_t p : params)
        if (i < r.size()) r[i++] = p;
    return r;
}

// Whether `reply` answers `request` (same device, feature, function and software ID).
inline bool Answers(const Report& reply, const Report& request) {
    return reply[0] == kLong && reply[1] == request[1] && reply[2] == request[2] && reply[3] == request[3];
}
// Whether `reply` is a HID++ 2.0 error for `request`.
inline bool Refuses(const Report& reply, const Report& request) {
    return reply[1] == request[1] && reply[2] == 0xFF && reply[3] == request[2] && reply[4] == request[3];
}

enum class Kind { Fixed, Breathing, Cycle, ColorWave };

struct Effect {
    Kind kind = Kind::Fixed;
    Rgb color{};
    uint16_t periodMs = 5000;
    uint8_t intensity = 100;  // 0-100
    bool operator==(const Effect& o) const {
        return kind == o.kind && color == o.color && periodMs == o.periodMs && intensity == o.intensity;
    }
    bool operator!=(const Effect& o) const { return !(*this == o); }
};

// Where a mouse keeps its effects: indexes in cluster 0's list (-1: missing), and whether the
// whole-mouse color wave (cluster FF, effect 0) is known to work on it.
struct Layout {
    int fixed = -1, breathing = -1, cycle = -1;
    bool colorWave = false;
    bool Has(Kind k) const {
        switch (k) {
        case Kind::Fixed: return fixed >= 0;
        case Kind::Breathing: return breathing >= 0;
        case Kind::Cycle: return cycle >= 0;
        case Kind::ColorWave: return colorWave;
        }
        return false;
    }
};

inline Report SetEffect(uint8_t device, uint8_t feature, const Layout& l, const Effect& e) {
    const uint8_t hi = static_cast<uint8_t>(e.periodMs >> 8), lo = static_cast<uint8_t>(e.periodMs);
    const uint8_t in = e.intensity > 100 ? 100 : e.intensity;
    const Rgb c = e.color;
    switch (e.kind) {
    case Kind::Fixed:
        return Request(device, feature, 1, {0x00, static_cast<uint8_t>(l.fixed), c.r, c.g, c.b, 0x02, 0, 0, 0, 0, 0, 0, 0x01});
    case Kind::Breathing:
        return Request(device, feature, 1,
                       {0x00, static_cast<uint8_t>(l.breathing), c.r, c.g, c.b, hi, lo, 0x00, in, 0, 0, 0, 0x01});
    case Kind::Cycle:
        return Request(device, feature, 1, {0x00, static_cast<uint8_t>(l.cycle), 0, 0, 0, 0, 0, hi, lo, in, 0, 0, 0x01});
    case Kind::ColorWave:
        return Request(device, feature, 1, {0xFF, 0x00, 0, 0, 0, 0, 0, 0, lo, 0x01, in, hi, 0x01});
    }
    return Report{};
}

// Period of one cycle for an effect speed (cycles per second), kept to what the mouse runs.
inline uint16_t PeriodMs(double speed) {
    const double ms = speed > 0 ? 1000.0 / speed : 20000.0;
    return static_cast<uint16_t>(ms < 1000 ? 1000 : ms > 20000 ? 20000 : ms + 0.5);
}

// The mouse's own effect that looks like LumaBridge's, if it has one. The rainbows only when
// they're the full, vivid rainbow (the mouse's can't be customized).
inline std::optional<Effect> ForEffect(const fx::Params& p, const Layout& l) {
    const bool fullRainbow = p.hueSpan >= 359.5 && p.saturation >= 0.999;
    Effect e;
    e.periodMs = PeriodMs(p.speed);
    switch (p.kind) {
    case fx::Kind::Breathing:
        if (p.speed <= 0) return std::nullopt;
        e.kind = Kind::Breathing;
        e.color = p.color1;
        break;
    case fx::Kind::ColorCycle:
        if (p.speed <= 0 || !fullRainbow) return std::nullopt;
        e.kind = Kind::Cycle;
        break;
    case fx::Kind::RainbowWave:
        if (p.speed <= 0 || !fullRainbow) return std::nullopt;
        e.kind = Kind::ColorWave;
        break;
    default: return std::nullopt;
    }
    if (!l.Has(e.kind)) return std::nullopt;
    return e;
}

}  // namespace luma::app::hidpp
