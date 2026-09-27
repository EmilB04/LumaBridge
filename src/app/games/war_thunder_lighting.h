// War Thunder lighting from the game's own local status page (http://127.0.0.1:8111, the
// page its browser map uses): /indicators says whether you're in a vehicle, which kind, and
// for tanks how much crew is left; /state has aircraft throttle and fuel. Pure C++, tested.
#pragma once

#include <cstdint>
#include <string>

#include "effects.h"
#include "json.h"

namespace luma::app::games {

class WarThunderLighting {
public:
    static constexpr uint64_t kStaleMs = 3000;

    void OnIndicators(const Json& j, uint64_t now) {
        lastSeen_ = now;
        valid_ = j["valid"].Bool(false);
        if (!valid_) return;
        army_ = j["army"].String();
        const double total = j["crew_total"].Number(0), current = j["crew_current"].Number(0);
        if (total > 0) {
            const double ratio = current / total;
            if (ratio < crew_ - 1e-6 && crew_ <= 1.0) crewLostAt_ = now;
            crew_ = ratio;
        } else {
            crew_ = 1.0;
        }
    }

    void OnState(const Json& j, uint64_t now) {
        if (!j["valid"].Bool(false)) return;
        (void)now;
        // Keys carry units ("throttle 1, %", "Mfuel, kg").
        throttle_ = j["throttle 1, %"].Number(0);
        const double fuel = j["Mfuel, kg"].Number(-1), full = j["Mfuel0, kg"].Number(0);
        fuel_ = full > 0 && fuel >= 0 ? fuel / full : 1.0;
    }

    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && valid_; }

    fx::Params Current(uint64_t now) const {
        using fx::Kind;
        if (crewLostAt_ && now - crewLostAt_ < 800) return Make(Kind::Strobe, {255, 0, 0}, {}, 6);
        if (army_ == "tank" || army_ == "ship") {
            if (crew_ <= 0) return Make(Kind::Static, {60, 0, 0});
            // Green at full crew, through yellow, to red.
            const double c = crew_ > 1 ? 1 : crew_;
            const Rgb color = c > 0.5 ? Rgb{static_cast<uint8_t>(255 * (1 - c) * 2), 255, 0}
                                      : Rgb{255, static_cast<uint8_t>(255 * c * 2), 0};
            return c < 0.34 ? Make(Kind::Breathing, color, {}, 1.4) : Make(Kind::Static, color);
        }
        if (fuel_ < 0.12) return Make(Kind::Breathing, {255, 170, 0}, {}, 1.2);
        if (throttle_ > 100.5) return Make(Kind::Twinkle, {255, 90, 0}, {255, 220, 120}, 2.0);  // WEP
        return Make(Kind::Gradient, {0, 90, 255}, {120, 200, 255}, 0.1);
    }

private:
    static fx::Params Make(fx::Kind k, Rgb c1, Rgb c2 = {}, double speed = 0) {
        fx::Params p;
        p.kind = k;
        p.color1 = c1;
        p.color2 = c2;
        p.speed = speed;
        return p;
    }

    uint64_t lastSeen_ = 0, crewLostAt_ = 0;
    bool valid_ = false;
    std::string army_;
    double crew_ = 1.0, throttle_ = 0, fuel_ = 1.0;
};

}  // namespace luma::app::games
