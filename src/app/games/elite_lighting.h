// Elite Dangerous lighting from the game's own Status.json, which it keeps up to date in
// Saved Games\Frontier Developments\Elite Dangerous (the file ship-panel tools read; nothing
// to switch on). Its "Flags" number carries the ship's state bit by bit:
//   0 docked, 1 landed, 2 gear down, 3 shields up, 4 supercruise, 6 hardpoints out,
//   8 lights on, 17 FSD charging, 19 low fuel, 20 overheating, 22 in danger,
//   23 being interdicted, 24 in the main ship, 30 FSD jump
// Pure C++, tested.
#pragma once

#include <cstdint>
#include <string>

#include "effects.h"
#include "json.h"

namespace luma::app::games {

class EliteLighting {
public:
    enum : uint64_t {
        kDocked = 1ull << 0, kLanded = 1ull << 1, kSupercruise = 1ull << 4, kHardpoints = 1ull << 6,
        kFsdCharging = 1ull << 17, kLowFuel = 1ull << 19, kOverheating = 1ull << 20, kInDanger = 1ull << 22,
        kInterdicted = 1ull << 23, kFsdJump = 1ull << 30,
    };

    // The text of Status.json. False if it isn't a status (the file is empty while the game
    // rewrites it).
    bool OnStatus(const std::string& text, uint64_t now) {
        Json j;
        if (text.empty() || !Json::Parse(text, &j) || j["event"].String() != "Status") return false;
        flags_ = static_cast<uint64_t>(j["Flags"].Number(0));
        lastSeen_ = now;
        return true;
    }

    // Status.json is only rewritten when something changes, so it has no "stale": the caller
    // Reset()s it when the game isn't running.
    bool Active() const { return lastSeen_ != 0; }
    void Reset() { lastSeen_ = 0; flags_ = 0; }

    fx::Params Current() const {
        using fx::Kind;
        if (flags_ & kOverheating) return Make(Kind::Strobe, {255, 0, 0}, 10);
        if (flags_ & kInterdicted) return Make(Kind::Strobe, {255, 40, 0}, 6);
        if (flags_ & kFsdJump) return Make(Kind::Strobe, {255, 255, 255}, 14);
        if (flags_ & kFsdCharging) return Make(Kind::Breathing, {120, 200, 255}, 3);
        if (flags_ & kInDanger) return Make(Kind::Breathing, {255, 0, 0}, 2);
        if (flags_ & kLowFuel) return Make(Kind::Breathing, {255, 170, 0}, 1.2);
        if (flags_ & (kDocked | kLanded)) return Make(Kind::Static, {0, 70, 130}, 0);
        if (flags_ & kHardpoints) return Make(Kind::Static, {255, 40, 0}, 0);
        if (flags_ & kSupercruise) return Make(Kind::Static, {0, 220, 255}, 0);
        return Make(Kind::Static, {255, 120, 0}, 0);  // normal space: the cockpit's orange
    }

    uint64_t flags() const { return flags_; }

private:
    static fx::Params Make(fx::Kind k, Rgb c, double speed) {
        fx::Params p;
        p.kind = k;
        p.color1 = c;
        p.speed = speed;
        return p;
    }

    uint64_t lastSeen_ = 0, flags_ = 0;
};

}  // namespace luma::app::games
