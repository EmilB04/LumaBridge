// Counter-Strike 2 lighting from Valve's Game State Integration (GSI): the game POSTs its
// state as JSON to LumaBridge (see gamestate_integration_lumabridge.cfg). Pure C++, tested.
//
// Layers, most important first:
//   bomb exploded / defused (2 s) > kill / headshot (0.7 s) > round won / lost (3 s) >
//   flashbang (fades with the flash) > burning > bomb planted (blinks faster over 40 s) >
//   dead > low health > freeze time > team color.
#pragma once

#include <cstdint>
#include <string>

#include "effects.h"
#include "json.h"

namespace luma::app::games {

class Cs2Lighting {
public:
    static constexpr Rgb kCt{40, 110, 255};
    static constexpr Rgb kT{255, 150, 20};
    static constexpr uint64_t kBombMs = 40000;
    static constexpr uint64_t kStaleMs = 35000;  // CS2 sends a heartbeat every ~30 s

    // One GSI payload. Returns false if it isn't one (or has the wrong auth token).
    bool OnState(const Json& j, uint64_t now, const std::string& token = "") {
        if (!j.IsObject()) return false;
        if (!token.empty() && j["auth"]["token"].String() != token) return false;
        lastSeen_ = now;
        const Json& player = j["player"];
        const std::string self = j["provider"]["steamid"].String();
        const bool own = !player["steamid"].IsString() || player["steamid"].String() == self;
        inMenu_ = player["activity"].String() == "menu" || !j["map"].IsObject();
        const std::string team = player["team"].String();
        if (own && !team.empty()) team_ = team;

        const Json& state = player["state"];
        if (own && state.IsObject()) {
            health_ = static_cast<int>(state["health"].Number(100));
            flashed_ = static_cast<int>(state["flashed"].Number(0));
            burning_ = static_cast<int>(state["burning"].Number(0));
            const int kills = static_cast<int>(state["round_kills"].Number(0));
            const int hs = static_cast<int>(state["round_killhs"].Number(0));
            if (hs > roundHs_) Pulse(&headshotUntil_, now, 700);
            else if (kills > roundKills_) Pulse(&killUntil_, now, 700);
            roundKills_ = kills;
            roundHs_ = hs;
        } else if (!own) {
            flashed_ = burning_ = 0;  // spectating someone else: their state isn't ours
        }

        const Json& round = j["round"];
        phase_ = round["phase"].String();
        const std::string bomb = round["bomb"].String();
        if (bomb == "planted" && bomb_ != "planted") bombPlantedAt_ = now;
        if (bomb == "exploded" && bomb_ != "exploded") Pulse(&explodedUntil_, now, 2000);
        if (bomb == "defused" && bomb_ != "defused") Pulse(&defusedUntil_, now, 2000);
        bomb_ = bomb;
        const std::string winner = round["win_team"].String();
        if (!winner.empty() && winner != winTeam_) {
            (winner == team_ ? wonUntil_ : lostUntil_) = now + 3000;
        }
        winTeam_ = winner;
        return true;
    }

    // Receiving data and in a match.
    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && !inMenu_; }

    fx::Params Current(uint64_t now) const {
        using fx::Kind;
        const Rgb teamColor = team_ == "T" ? kT : kCt;
        if (now < explodedUntil_) return Make(Kind::Strobe, {255, 90, 0}, {}, 6);
        if (now < defusedUntil_) return Make(Kind::Strobe, kCt, {}, 4);
        if (now < headshotUntil_) return Make(Kind::Static, {255, 215, 0});
        if (now < killUntil_) return Make(Kind::Static, {40, 255, 80});
        if (now < wonUntil_) return Make(Kind::RainbowWave, teamColor, {}, 1.0);
        if (now < lostUntil_) return Make(Kind::Breathing, {160, 0, 0}, {}, 1.0);
        if (flashed_ > 0) {
            const double k = flashed_ / 255.0;
            const Rgb base = health_ > 0 ? teamColor : Rgb{};
            return Make(Kind::Static, Rgb{Mix(base.r, 255, k), Mix(base.g, 255, k), Mix(base.b, 255, k)});
        }
        if (burning_ > 0) return Make(Kind::Twinkle, {255, 60, 0}, {255, 200, 0}, 2.5);
        if (bomb_ == "planted") {
            // 1 blink per second, speeding up to 5 as the 40 s fuse runs out.
            const double t = now > bombPlantedAt_ ? double(now - bombPlantedAt_) / kBombMs : 0.0;
            return Make(Kind::Strobe, {255, 0, 0}, {}, 1.0 + 4.0 * (t > 1 ? 1 : t * t));
        }
        if (health_ <= 0) return Make(Kind::Static, Scale(teamColor, 0.12));
        if (health_ <= 25) return Make(Kind::Breathing, {255, 0, 0}, {}, 1.6);
        if (phase_ == "freezetime") return Make(Kind::Breathing, teamColor, {}, 0.6);
        return Make(Kind::Static, teamColor);
    }

    const std::string& team() const { return team_; }

private:
    static fx::Params Make(fx::Kind k, Rgb c1, Rgb c2 = {}, double speed = 0) {
        fx::Params p;
        p.kind = k;
        p.color1 = c1;
        p.color2 = c2;
        p.speed = speed;
        return p;
    }
    static uint8_t Mix(uint8_t a, uint8_t b, double k) { return static_cast<uint8_t>(a + (b - a) * k + 0.5); }
    static void Pulse(uint64_t* until, uint64_t now, uint64_t ms) { *until = now + ms; }

    uint64_t lastSeen_ = 0;
    bool inMenu_ = true;
    std::string team_ = "CT";
    std::string phase_, bomb_, winTeam_;
    int health_ = 100, flashed_ = 0, burning_ = 0, roundKills_ = 0, roundHs_ = 0;
    uint64_t bombPlantedAt_ = 0, explodedUntil_ = 0, defusedUntil_ = 0, killUntil_ = 0, headshotUntil_ = 0,
             wonUntil_ = 0, lostUntil_ = 0;
};

}  // namespace luma::app::games
