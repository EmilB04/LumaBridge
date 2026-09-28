// Forza Horizon 4 / 5 and Forza Motorsport lighting from the games' own "Data Out" telemetry:
// once switched on in the game's settings (Data Out IP 127.0.0.1, the port LumaBridge shows),
// the game sends a UDP packet many times a second. LumaBridge reads only the start, which is
// the same in every Forza format:
//   s32 IsRaceOn, u32 TimestampMS, f32 EngineMaxRpm, f32 EngineIdleRpm, f32 CurrentEngineRpm
// and turns the engine speed into rev lights. Pure C++, tested.
#pragma once

#include <cstdint>
#include <cstring>

#include "effects.h"

namespace luma::app::games {

class ForzaLighting {
public:
    static constexpr uint16_t kDefaultPort = 5300;
    static constexpr uint64_t kStaleMs = 1000;

    // One Data Out packet. False if it's too short to be one.
    bool OnPacket(const uint8_t* p, size_t n, uint64_t now) {
        if (n < 20) return false;
        int32_t raceOn;
        float maxRpm, idleRpm, rpm;
        std::memcpy(&raceOn, p, 4);
        std::memcpy(&maxRpm, p + 8, 4);
        std::memcpy(&idleRpm, p + 12, 4);
        std::memcpy(&rpm, p + 16, 4);
        lastSeen_ = now;
        racing_ = raceOn != 0;
        const float span = maxRpm - idleRpm;
        revs_ = span > 100 && rpm > 0 ? (rpm - idleRpm) / span : 0;
        if (revs_ < 0) revs_ = 0;
        if (revs_ > 1) revs_ = 1;
        return true;
    }

    // Driving (not in the menus or paused) and sending.
    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && racing_; }

    // Rev lights: calm blue at low revs, then green, yellow and red as the engine climbs, and a
    // fast red flash at the limiter (time to shift).
    fx::Params Current() const {
        fx::Params p;
        p.speed = 0;
        if (revs_ >= 0.95) {
            p.kind = fx::Kind::Strobe;
            p.color1 = Rgb{255, 0, 0};
            p.speed = 12;
            return p;
        }
        p.kind = fx::Kind::Static;
        const double r = revs_;
        if (r < 0.5) p.color1 = Rgb{0, 90, 255};
        else if (r < 0.7) p.color1 = Blend(Rgb{0, 90, 255}, Rgb{0, 255, 60}, (r - 0.5) / 0.2);
        else if (r < 0.85) p.color1 = Blend(Rgb{0, 255, 60}, Rgb{255, 220, 0}, (r - 0.7) / 0.15);
        else p.color1 = Blend(Rgb{255, 220, 0}, Rgb{255, 0, 0}, (r - 0.85) / 0.1);
        return p;
    }

    double revs() const { return revs_; }

private:
    static Rgb Blend(Rgb a, Rgb b, double t) {
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        return Rgb{static_cast<uint8_t>(a.r + (b.r - a.r) * t), static_cast<uint8_t>(a.g + (b.g - a.g) * t),
                   static_cast<uint8_t>(a.b + (b.b - a.b) * t)};
    }

    uint64_t lastSeen_ = 0;
    bool racing_ = false;
    double revs_ = 0;
};

}  // namespace luma::app::games
