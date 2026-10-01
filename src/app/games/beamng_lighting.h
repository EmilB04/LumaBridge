// BeamNG.drive lighting from the game's own OutGauge output (the Live for Speed format):
// switched on in the game (Options > Other > Protocols: OutGauge, IP 127.0.0.1, the port
// LumaBridge shows, 4444 by default), the game sends a UDP packet many times a second:
//   u32 time, char car[4], u16 flags, u8 gear, u8 plid, f32 speed, f32 rpm, f32 turbo,
//   f32 engTemp, f32 fuel, f32 oilPressure, f32 oilTemp, u32 dashLights, u32 showLights, ...
// OutGauge carries no redline, so it's the highest engine speed seen this session. The shift
// light (showLights bit 0) is the limiter flash; the oil and battery lights are an amber
// pulse. Pure C++, tested; not checked against the running game.
#pragma once

#include <cstdint>
#include <cstring>

#include "effects.h"
#include "rev_lights.h"

namespace luma::app::games {

class BeamNgLighting {
public:
    static constexpr uint16_t kDefaultPort = 4444;
    static constexpr uint64_t kStaleMs = 1000;
    static constexpr size_t kMinSize = 92;
    static constexpr uint32_t kShift = 1u << 0, kOil = 1u << 8, kBattery = 1u << 9;

    bool OnPacket(const uint8_t* p, size_t n, uint64_t now) {
        if (n < kMinSize) return false;
        float rpm;
        uint32_t show;
        std::memcpy(&rpm, p + 16, 4);
        std::memcpy(&show, p + 44, 4);
        if (!(rpm >= 0 && rpm < 30000)) return false;  // not an OutGauge packet
        lastSeen_ = now;
        rpm_ = rpm;
        if (rpm > peak_) peak_ = rpm;
        show_ = show;
        return true;
    }

    // The engine running, and sending.
    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && rpm_ > 50; }

    fx::Params Current() const {
        if (!(show_ & kShift) && (show_ & (kOil | kBattery))) {
            fx::Params p;
            p.kind = fx::Kind::Breathing;
            p.color1 = Rgb{255, 170, 0};
            p.speed = 1.5;
            return p;
        }
        return RevLights(Clamp01(rpm_ / (peak_ > 3000 ? peak_ : 3000)), (show_ & kShift) != 0);
    }

private:
    uint64_t lastSeen_ = 0;
    double rpm_ = 0, peak_ = 0;
    uint32_t show_ = 0;
};

}  // namespace luma::app::games
