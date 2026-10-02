// Assetto Corsa / Assetto Corsa Competizione / Assetto Corsa EVO lighting from the games' shared
// memory telemetry: while they run, the games publish three named memory blocks for telemetry
// apps (dashboards, wheel displays), documented by Kunos ("Shared Memory API", #pragma pack(4)).
// LumaBridge opens them read-only by name; nothing touches the game itself.
//   Local\acpmf_physics   i32 packetId (0), ... i32 gear (16), i32 rpms (20), ...
//                         i32 pitLimiterOn (248)
//   Local\acpmf_graphics  i32 packetId (0), i32 status (4): 0 off, 1 replay, 2 live, 3 pause
//   Local\acpmf_static    ... i32 maxRpm (412)
// Assetto Corsa EVO names them acevo_pmf_physics / _graphics / _static and keeps the same
// physics start and graphics status; its redline moved to physics i32 currentMaxRpm (588).
// Pure C++, tested with blocks built to those offsets; not checked against the running games.
#pragma once

#include <cstdint>
#include <cstring>

#include "effects.h"
#include "rev_lights.h"

namespace luma::app::games {

class AcLighting {
public:
    static constexpr uint64_t kStaleMs = 1000;
    static constexpr size_t kPhysicsMin = 252, kGraphicsMin = 8, kStaticMin = 416, kEvoPhysicsMin = 592;
    static constexpr int32_t kLive = 2;

    // One look at the three blocks (any of them may be missing: nullptr); `evo`: Assetto Corsa
    // EVO's blocks. False if they don't hold a car on track.
    bool OnBlocks(const uint8_t* physics, size_t pn, const uint8_t* graphics, size_t gn, const uint8_t* stat, size_t sn,
                  uint64_t now, bool evo = false) {
        if (!physics || pn < (evo ? kEvoPhysicsMin : kPhysicsMin) || !graphics || gn < kGraphicsMin) return false;
        int32_t packet, rpm, limiter, status, maxRpm = 0;
        std::memcpy(&packet, physics, 4);
        std::memcpy(&rpm, physics + 20, 4);
        std::memcpy(&limiter, physics + 248, 4);
        std::memcpy(&status, graphics + 4, 4);
        if (evo) std::memcpy(&maxRpm, physics + 588, 4);
        else if (stat && sn >= kStaticMin) std::memcpy(&maxRpm, stat + 412, 4);
        if (status != kLive || rpm < 0 || rpm > 30000) {
            live_ = false;
            return false;
        }
        // The physics packet counts up while the car moves; a frozen one is a game that stopped
        // writing (closed to the menus, or gone).
        if (packet != lastPacket_ || !lastSeen_) {
            lastPacket_ = packet;
            lastSeen_ = now;
        }
        live_ = true;
        rpm_ = rpm;
        if (rpm > peak_) peak_ = rpm;
        maxRpm_ = maxRpm > 1000 ? maxRpm : 0;
        limiter_ = limiter != 0;
        return true;
    }

    // On track, the engine running, and the blocks still being written.
    bool Active(uint64_t now) const { return live_ && lastSeen_ && now - lastSeen_ < kStaleMs && rpm_ > 50; }

    fx::Params Current() const {
        if (limiter_) {
            fx::Params p;
            p.kind = fx::Kind::Breathing;
            p.color1 = Rgb{255, 170, 0};
            p.speed = 1.5;
            return p;
        }
        // The car's own redline when the game gives one, else the highest engine speed seen.
        const double top = maxRpm_ > 0 ? maxRpm_ : (peak_ > 3000 ? peak_ : 3000);
        return RevLights(Clamp01(rpm_ / top), false);
    }

    void Reset() { *this = AcLighting(); }

private:
    uint64_t lastSeen_ = 0;
    int32_t lastPacket_ = 0;
    bool live_ = false, limiter_ = false;
    double rpm_ = 0, peak_ = 0, maxRpm_ = 0;
};

}  // namespace luma::app::games
