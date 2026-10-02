// Bomb timing from CS2's official GSI. Normal play supplies a plant event;
// spectator feeds can also supply seconds until detonation.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string>
#include "json.h"
namespace luma::app::games {
struct BombCountdown {
    bool active = false, estimated = true;
    int seconds = 0;
    bool testing = false;
};
// A short, explicitly requested display check; never inserted into the game feed.
inline BombCountdown TestBombCountdown(uint64_t now, uint64_t until) {
    if (now >= until) return {};
    const uint64_t remaining = std::min<uint64_t>(until - now, 10000);
    return {true, false, static_cast<int>((remaining + 999) / 1000), true};
}
class Cs2BombTimer {
public:
    // Called after the enclosing feed has authenticated the payload.
    void OnState(const Json& j, uint64_t now) {
        const auto& map = j["map"];
        const std::string name = map["name"].String();
        const double mapRound = map["round"].Number(-1);
        const int round = std::isfinite(mapRound) && mapRound >= 0 && mapRound <= 1000000
                              ? static_cast<int>(mapRound) : -1;
        if (!map.IsObject() || (lastSeen_ && now - lastSeen_ >= kStaleMs) || name != map_ || round != round_)
            planted_ = exact_ = false;
        map_ = name; round_ = round; lastSeen_ = now;
        const std::string phase = j["round"]["phase"].String();
        const std::string state = j["bomb"]["state"].String();
        const std::string bomb = j["round"]["bomb"].String();
        if (!map.IsObject() || j["player"]["activity"].String() == "menu" || phase == "over" ||
            phase == "freezetime" || bomb == "defused" || bomb == "exploded" ||
            state == "defused" || state == "exploded" || state == "carried" || state == "dropped" || state == "planting") {
            planted_ = exact_ = false;
            return;
        }
        const auto& countdown = j["phase_countdowns"];
        const bool bombPhase = countdown["phase"].String() == "bomb";
        if (bomb != "planted" && state != "planted" && state != "defusing" && !bombPhase) {
            planted_ = exact_ = false;
            return;
        }
        if (!planted_) { plantedAt_ = now; planted_ = true; exact_ = false; }
        // While defusing, bomb.countdown may describe the defuse rather than the fuse.
        double seconds = bombPhase ? Seconds(countdown["phase_ends_in"]) : -1;
        if (seconds < 0 && state == "planted") seconds = Seconds(j["bomb"]["countdown"]);
        if (seconds >= 0) {
            exactMs_ = static_cast<uint64_t>(std::ceil(seconds * 1000));
            exactAt_ = now; exact_ = true;
        }
    }
    BombCountdown Current(uint64_t now, int fuseSeconds = 40) const {
        if (!planted_ || now < lastSeen_ || now - lastSeen_ >= kStaleMs) return {};
        const uint64_t start = exact_ ? exactAt_ : plantedAt_;
        const uint64_t duration = exact_ ? exactMs_ : std::clamp(fuseSeconds, 10, 120) * 1000ULL;
        const uint64_t elapsed = now >= start ? now - start : 0;
        if (elapsed >= duration) return {};
        return {true, !exact_, static_cast<int>((duration - elapsed + 999) / 1000)};
    }
private:
    static double Seconds(const Json& value) {
        double seconds = -1;
        if (value.IsNumber()) seconds = value.Number(-1);
        else if (value.IsString() && !value.String().empty()) {
            char* end = nullptr;
            seconds = std::strtod(value.String().c_str(), &end);
            if (end != value.String().c_str() + value.String().size()) return -1;
        }
        return std::isfinite(seconds) && seconds >= 0 && seconds <= 120 ? seconds : -1;
    }
    static constexpr uint64_t kStaleMs = 35000;
    bool planted_ = false, exact_ = false;
    uint64_t plantedAt_ = 0, lastSeen_ = 0, exactAt_ = 0, exactMs_ = 0;
    std::string map_;
    int round_ = -1;
};
} // namespace luma::app::games
