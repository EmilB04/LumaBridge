// Dota 2 lighting from Valve's Game State Integration (the same mechanism as CS2: Dota 2
// POSTs its state as JSON to LumaBridge, set up by a cfg file in its
// game/dota/cfg/gamestate_integration folder, with -gamestateintegration in its launch
// options). Pure C++, tested.
//
// What shows, most important first:
//   win / loss at the end (4 s) > a kill (0.8 s) > dead (dim team color) > stunned or hexed
//   (white pulse) > low health (red breathing) > picking / waiting for the horn (team color
//   breathing) > in the game: your team's color, dimmer at night.
#pragma once

#include <cstdint>
#include <string>

#include "effects.h"
#include "json.h"

namespace luma::app::games {

class Dota2Lighting {
public:
    static constexpr Rgb kRadiant{40, 210, 70};
    static constexpr Rgb kDire{230, 50, 40};
    static constexpr uint64_t kStaleMs = 40000;  // GSI sends a heartbeat every 10 s at least

    // One POST. False if it isn't Dota 2's (or the token is wrong).
    bool OnState(const Json& j, uint64_t now, const char* token) {
        if (j["provider"]["appid"].Number(0) != 570) return false;
        if (token && j["auth"]["token"].String() != token) return false;
        lastSeen_ = now;
        const Json& map = j["map"];
        state_ = map["game_state"].String();
        day_ = map["daytime"].Bool(true);
        const Json& player = j["player"];
        const std::string team = player["team_name"].String();
        if (team == "radiant" || team == "dire") team_ = team;
        const int kills = static_cast<int>(player["kills"].Number(-1));
        if (kills > kills_ && kills_ >= 0) killUntil_ = now + 800;
        kills_ = kills;
        const Json& hero = j["hero"];
        alive_ = hero["alive"].Bool(true);
        health_ = hero["health_percent"].Number(100);
        disabled_ = hero["stunned"].Bool(false) || hero["hexed"].Bool(false);
        const std::string winner = map["win_team"].String();
        if ((winner == "radiant" || winner == "dire") && winner != winTeam_) endedAt_ = now;
        winTeam_ = winner;
        return true;
    }

    // In a match (from hero selection until a while after the end).
    bool Active(uint64_t now) const {
        if (!lastSeen_ || now - lastSeen_ > kStaleMs) return false;
        return state_.rfind("DOTA_GAMERULES_STATE_", 0) == 0 && state_ != "DOTA_GAMERULES_STATE_INIT" &&
               state_ != "DOTA_GAMERULES_STATE_DISCONNECT" &&
               !(state_ == "DOTA_GAMERULES_STATE_POST_GAME" && endedAt_ && now - endedAt_ > 15000);
    }

    fx::Params Current(uint64_t now) const {
        using fx::Kind;
        const Rgb team = team_ == "dire" ? kDire : kRadiant;
        if (endedAt_ && now - endedAt_ < 4000) {
            if (winTeam_ == team_) return Make(Kind::RainbowWave, team, {}, 0.8);
            return Make(Kind::Breathing, {120, 0, 0}, {}, 0.6);
        }
        if (now < killUntil_) return Make(Kind::Strobe, {255, 200, 40}, {}, 6);
        if (state_ == "DOTA_GAMERULES_STATE_HERO_SELECTION" || state_ == "DOTA_GAMERULES_STATE_STRATEGY_TIME" ||
            state_ == "DOTA_GAMERULES_STATE_WAIT_FOR_PLAYERS_TO_LOAD" || state_ == "DOTA_GAMERULES_STATE_PRE_GAME" ||
            state_ == "DOTA_GAMERULES_STATE_TEAM_SHOWCASE")
            return Make(Kind::Breathing, team, {}, 0.4);
        if (!alive_) return Make(Kind::Static, Scale(team, 0.12));
        if (disabled_) return Make(Kind::Breathing, {230, 230, 255}, {}, 2.5);
        if (health_ <= 25) return Make(Kind::Breathing, {255, 0, 0}, {}, 1.4);
        return Make(Kind::Static, day_ ? team : Scale(team, 0.45));
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

    uint64_t lastSeen_ = 0, killUntil_ = 0, endedAt_ = 0;
    std::string state_, team_ = "radiant", winTeam_ = "none";
    bool day_ = true, alive_ = true, disabled_ = false;
    double health_ = 100;
    int kills_ = -1;
};

}  // namespace luma::app::games
