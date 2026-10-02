// RaceRoom Racing Experience lighting from the game's shared memory telemetry: while it runs,
// RaceRoom publishes the "$R3E" memory block for telemetry apps (Sector3's public r3e-api,
// version 3.x, tightly packed). LumaBridge opens it read-only by name; nothing touches the game.
//   i32 version_major (0), ... i32 game_paused (20), i32 game_in_menus (24),
//   i32 game_in_replay (28), ... f32 engine_rps (1396), f32 max_engine_rps (1400),
//   f32 upshift_rps (1404), ... i32 pit_limiter (1572)
// (engine speeds in radians per second; the offsets are those of r3e.h 3.5). Pure C++, tested;
// not checked against the running game.
#pragma once

#include <cstdint>
#include <cstring>

#include "effects.h"
#include "rev_lights.h"

namespace luma::app::games {

class RaceRoomLighting {
public:
    static constexpr uint64_t kStaleMs = 1000;
    static constexpr size_t kMinSize = 1576;
    static constexpr int32_t kVersionMajor = 3;

    // One look at the block. False if it isn't a version-3 block with a car on track.
    bool OnBlock(const uint8_t* p, size_t n, uint64_t now) {
        if (!p || n < kMinSize) return false;
        int32_t major, paused, menus, replay, limiter;
        float rps, maxRps, upshift;
        std::memcpy(&major, p, 4);
        std::memcpy(&paused, p + 20, 4);
        std::memcpy(&menus, p + 24, 4);
        std::memcpy(&replay, p + 28, 4);
        std::memcpy(&rps, p + 1396, 4);
        std::memcpy(&maxRps, p + 1400, 4);
        std::memcpy(&upshift, p + 1404, 4);
        std::memcpy(&limiter, p + 1572, 4);
        driving_ = major == kVersionMajor && !paused && !menus && !replay && rps > 5 && rps < 3000;
        if (!driving_) return false;
        lastSeen_ = now;
        rps_ = rps;
        maxRps_ = maxRps > 50 ? maxRps : 0;
        upshift_ = upshift > 50 ? upshift : 0;
        limiter_ = limiter == 1;
        return true;
    }

    bool Active(uint64_t now) const { return driving_ && lastSeen_ && now - lastSeen_ < kStaleMs; }

    fx::Params Current() const {
        if (limiter_) {
            fx::Params p;
            p.kind = fx::Kind::Breathing;
            p.color1 = Rgb{255, 170, 0};
            p.speed = 1.5;
            return p;
        }
        const bool shift = upshift_ > 0 && rps_ >= upshift_;
        return RevLights(maxRps_ > 0 ? Clamp01(rps_ / maxRps_) : 0, shift);
    }

    void Reset() { *this = RaceRoomLighting(); }

private:
    uint64_t lastSeen_ = 0;
    bool driving_ = false, limiter_ = false;
    double rps_ = 0, maxRps_ = 0, upshift_ = 0;
};

}  // namespace luma::app::games
