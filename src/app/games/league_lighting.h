// League of Legends lighting from Riot's Live Client Data API: while you're in a match, the
// game answers https://127.0.0.1:2999/liveclientdata/allgamedata (read-only JSON: your champion,
// every player, the match's events). Official and always on; nothing to set up. Pure C++,
// tested.
//
// What shows, most important first:
//   victory / defeat (6 s) > your multikill (1.5 s) > your kill (0.8 s) > your team's dragon /
//   baron / herald (2 s, the dragon's color) > dead (dim team color) > low health (red
//   breathing) > your team's color (blue side / red side).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "effects.h"
#include "json.h"

namespace luma::app::games {

class LeagueLighting {
public:
    static constexpr Rgb kBlueSide{30, 110, 255};  // ORDER
    static constexpr Rgb kRedSide{255, 50, 50};    // CHAOS
    static constexpr uint64_t kStaleMs = 3000;

    // One allgamedata answer.
    void OnGameData(const Json& j, uint64_t now) {
        const Json& me = j["activePlayer"];
        if (!me.IsObject()) return;
        lastSeen_ = now;
        const Json& stats = me["championStats"];
        const double hp = stats["currentHealth"].Number(1), maxHp = stats["maxHealth"].Number(1);
        health_ = maxHp > 0 ? hp / maxHp : 1;
        // Who I am, under every name the API uses.
        names_.clear();
        for (const char* k : {"riotId", "riotIdGameName", "summonerName"})
            if (!me[k].String().empty()) names_.push_back(me[k].String());
        const Json& players = j["allPlayers"];
        teamOf_.clear();
        for (size_t i = 0; i < players.size(); ++i) {
            const Json& p = players[i];
            const std::string team = p["team"].String();
            bool mine = false;
            for (const char* k : {"riotId", "riotIdGameName", "summonerName"}) {
                const std::string& n = p[k].String();
                if (n.empty()) continue;
                teamOf_.push_back({n, team});
                mine = mine || IsMe(n);
            }
            if (mine) {
                team_ = team;
                dead_ = p["isDead"].Bool(false);
            }
        }
        const Json& events = j["events"]["Events"];
        for (size_t i = 0; i < events.size(); ++i) OnEvent(events[i], now);
    }

    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs; }

    fx::Params Current(uint64_t now) const {
        using fx::Kind;
        const Rgb team = team_ == "CHAOS" ? kRedSide : kBlueSide;
        if (now < endUntil_) return won_ ? Make(Kind::RainbowWave, team, {}, 0.8) : Make(Kind::Breathing, {120, 0, 0}, {}, 0.6);
        if (now < multiUntil_) return Make(Kind::RainbowWave, team, {}, 1.5);
        if (now < killUntil_) return Make(Kind::Strobe, {255, 200, 40}, {}, 6);
        if (now < objectiveUntil_) return Make(Kind::Breathing, objectiveColor_, {}, 1.5);
        if (dead_) return Make(Kind::Static, Scale(team, 0.12));
        if (health_ <= 0.25) return Make(Kind::Breathing, {255, 0, 0}, {}, 1.4);
        return Make(Kind::Static, team);
    }

    const std::string& team() const { return team_; }

private:
    bool IsMe(const std::string& n) const {
        for (const std::string& m : names_)
            if (m == n) return true;
        return false;
    }
    std::string TeamOf(const std::string& n) const {
        for (const auto& [name, team] : teamOf_)
            if (name == n) return team;
        return {};
    }

    void OnEvent(const Json& e, uint64_t now) {
        const int id = static_cast<int>(e["EventID"].Number(-1));
        if (id <= lastEvent_) return;  // seen already
        lastEvent_ = id;
        if (!primed_) return;  // events from before LumaBridge was looking: history only
        const std::string& name = e["EventName"].String();
        const std::string& killer = e["KillerName"].String();
        if (name == "ChampionKill" && IsMe(killer)) killUntil_ = now + 800;
        else if (name == "Multikill" && IsMe(killer)) multiUntil_ = now + 1500;
        else if ((name == "DragonKill" || name == "BaronKill" || name == "HeraldKill") && !team_.empty() &&
                 TeamOf(killer) == team_) {
            objectiveUntil_ = now + 2000;
            objectiveColor_ = name == "BaronKill" ? Rgb{160, 60, 255} : name == "HeraldKill" ? Rgb{180, 80, 255}
                                                                                         : DragonColor(e["DragonType"].String());
        } else if (name == "GameEnd") {
            endUntil_ = now + 6000;
            won_ = e["Result"].String() == "Win";
        }
    }

public:
    // The first answer only records how far the events go (a match joined midway shouldn't
    // replay its kills); call after it.
    void Prime() { primed_ = true; }

private:
    static Rgb DragonColor(const std::string& type) {
        if (type == "Fire") return {255, 90, 0};
        if (type == "Water") return {0, 170, 255};
        if (type == "Earth") return {170, 110, 40};
        if (type == "Air") return {220, 240, 255};
        if (type == "Hextech") return {0, 230, 230};
        if (type == "Chemtech") return {120, 255, 40};
        if (type == "Elder") return {200, 120, 255};
        return {255, 160, 0};
    }
    static fx::Params Make(fx::Kind k, Rgb c1, Rgb c2 = {}, double speed = 0) {
        fx::Params p;
        p.kind = k;
        p.color1 = c1;
        p.color2 = c2;
        p.speed = speed;
        return p;
    }

    uint64_t lastSeen_ = 0, killUntil_ = 0, multiUntil_ = 0, objectiveUntil_ = 0, endUntil_ = 0;
    std::vector<std::string> names_;
    std::vector<std::pair<std::string, std::string>> teamOf_;
    std::string team_ = "ORDER";
    Rgb objectiveColor_{255, 160, 0};
    double health_ = 1;
    bool dead_ = false, won_ = false, primed_ = false;
    int lastEvent_ = -1;
};

}  // namespace luma::app::games
