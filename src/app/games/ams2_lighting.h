// Automobilista 2 / Project CARS 2 lighting from the games' own UDP telemetry (the "Project
// CARS 2" format): switched on in the game (Options > System > Shared Memory: Project CARS 2
// UDP, a frame rate, IP 127.0.0.1 is automatic, port 5606). LumaBridge reads only the
// telemetry packet (type 0):
//   PacketBase (12 bytes, u8 packetType at 10), ... u8 carFlags (17), ... u16 rpm (40),
//   u16 maxRpm (42)
// carFlags: 2 engine active, 4 engine warning, 8 speed limiter. Pure C++, tested; the layout
// follows the public Project CARS 2 UDP documentation and isn't checked against the games.
#pragma once

#include <cstdint>
#include <cstring>

#include "effects.h"
#include "rev_lights.h"

namespace luma::app::games {

class Ams2Lighting {
public:
    static constexpr uint16_t kDefaultPort = 5606;
    static constexpr uint64_t kStaleMs = 1000;
    static constexpr size_t kMinSize = 44;
    static constexpr uint8_t kEngineActive = 2, kEngineWarning = 4, kLimiter = 8;

    bool OnPacket(const uint8_t* p, size_t n, uint64_t now) {
        if (n < kMinSize || p[10] != 0) return false;
        uint16_t rpm, maxRpm;
        std::memcpy(&rpm, p + 40, 2);
        std::memcpy(&maxRpm, p + 42, 2);
        if (maxRpm == 0) return false;
        lastSeen_ = now;
        flags_ = p[17];
        revs_ = Clamp01(static_cast<double>(rpm) / maxRpm);
        return true;
    }

    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && (flags_ & kEngineActive); }

    fx::Params Current() const {
        if (!(flags_ & kLimiter) && (flags_ & kEngineWarning)) {
            fx::Params p;
            p.kind = fx::Kind::Breathing;
            p.color1 = Rgb{255, 170, 0};
            p.speed = 1.5;
            return p;
        }
        return RevLights(revs_, (flags_ & kLimiter) != 0);
    }

private:
    uint64_t lastSeen_ = 0;
    uint8_t flags_ = 0;
    double revs_ = 0;
};

}  // namespace luma::app::games
