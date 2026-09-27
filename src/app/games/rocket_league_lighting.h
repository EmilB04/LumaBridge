// Rocket League lighting from Psyonix's Stats API: the game streams JSON events
// ({"Event": "...", "Data": {...}}, Data sometimes itself a JSON string) on a local port once
// PacketSendRate is set in TAGame\Config\DefaultStatsAPI.ini. Pure C++, tested.
//
// In a match both team colors run around each fan; a goal flashes the scoring team's color,
// then a comet of it chases around; overtime speeds things up; the winner's color breathes
// when the match ends. Unknown events are ignored (and counted, for the log).
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "effects.h"
#include "json.h"

namespace luma::app::games {

// Takes complete top-level JSON objects off the front of `buffer` (a TCP stream may carry
// several, or half of one). Braces inside strings are handled.
inline void TakeJsonObjects(std::string* buffer, std::vector<std::string>* out) {
    size_t start = std::string::npos, consumed = 0;
    int depth = 0;
    bool inString = false, escape = false;
    for (size_t i = 0; i < buffer->size(); ++i) {
        const char c = (*buffer)[i];
        if (inString) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"' && depth > 0) inString = true;
        else if (c == '{') {
            if (depth++ == 0) start = i;
        } else if (c == '}' && depth > 0 && --depth == 0) {
            out->push_back(buffer->substr(start, i + 1 - start));
            consumed = i + 1;
        }
    }
    if (depth == 0) consumed = buffer->size();  // only whitespace / junk left
    buffer->erase(0, consumed);
    if (buffer->size() > (1u << 20)) buffer->clear();  // runaway: resync
}

// DefaultStatsAPI.ini ([TAGame.MatchStatsExporter_TA] Port=49123, PacketSendRate=0): the
// value of `key` (case-insensitive), or `fallback`.
inline int StatsIniValue(const std::string& ini, const char* key, int fallback) {
    std::string lower = ini, k = key;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t pos = 0;
    while ((pos = lower.find(k, pos)) != std::string::npos) {
        const bool lineStart = pos == 0 || lower[pos - 1] == '\n' || lower[pos - 1] == '\r';
        size_t eq = pos + k.size();
        while (eq < lower.size() && lower[eq] == ' ') ++eq;
        if (lineStart && eq < lower.size() && lower[eq] == '=') return std::atoi(ini.c_str() + eq + 1);
        pos += k.size();
    }
    return fallback;
}

// The ini with PacketSendRate set to `rate` (added to the section if missing).
inline std::string WithPacketSendRate(const std::string& ini, int rate) {
    const std::string line = "PacketSendRate=" + std::to_string(rate);
    std::string lower = ini;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t pos = 0;
    while ((pos = lower.find("packetsendrate", pos)) != std::string::npos) {
        if (pos == 0 || lower[pos - 1] == '\n' || lower[pos - 1] == '\r') {
            size_t end = ini.find_first_of("\r\n", pos);
            if (end == std::string::npos) end = ini.size();
            return ini.substr(0, pos) + line + ini.substr(end);
        }
        ++pos;
    }
    const std::string section = "[TAGame.MatchStatsExporter_TA]";
    const size_t sec = ini.find(section);
    const std::string nl = ini.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    if (sec == std::string::npos) return ini + (ini.empty() || ini.back() == '\n' ? "" : nl) + section + nl + line + nl;
    size_t after = ini.find('\n', sec);
    after = after == std::string::npos ? ini.size() : after + 1;
    return ini.substr(0, after) + line + nl + ini.substr(after);
}

class RocketLeagueLighting {
public:
    static constexpr Rgb kBlue{24, 115, 255};
    static constexpr Rgb kOrange{255, 110, 0};
    static constexpr uint64_t kStaleMs = 10000;

    // Splits a raw message into event + data and handles it. False if it isn't one.
    bool OnMessage(const Json& msg, uint64_t now) {
        if (!msg.IsObject() || !msg["Event"].IsString()) return false;
        const Json& data = msg["Data"];
        if (data.IsString()) {  // double-encoded
            Json inner;
            if (Json::Parse(data.String(), &inner)) return OnEvent(msg["Event"].String(), inner, now);
        }
        return OnEvent(msg["Event"].String(), data, now);
    }

    bool OnEvent(const std::string& event, const Json& data, uint64_t now) {
        lastSeen_ = now;
        if (event == "UpdateState") {
            const Json& game = data["Game"];
            if (game.IsObject()) {
                inMatch_ = true;
                overtime_ = game["bOvertime"].Bool(false);
                const Json& teams = game["Teams"];
                for (size_t i = 0; i < teams.size() && i < 2; ++i) {
                    const int num = static_cast<int>(teams[i]["TeamNum"].Number(static_cast<double>(i)));
                    Rgb c;
                    if (ParseHex(teams[i]["ColorPrimary"].String(), &c) && num >= 0 && num < 2) teamColor_[num] = c;
                }
            }
        } else if (event == "GoalScored") {
            const Json& scorer = data["Scorer"];
            const int team = static_cast<int>(scorer["TeamNum"].Number(data["TeamNum"].Number(-1)));
            if (team == 0 || team == 1) {
                goalTeam_ = team;
                goalAt_ = now;
            }
        } else if (event == "MatchCreated" || event == "MatchInitialized" || event == "RoundStarted" ||
                   event == "CountdownBegin") {
            inMatch_ = true;
            endedAt_ = 0;
        } else if (event == "MatchEnded") {
            const int w = static_cast<int>(data["WinnerTeamNum"].Number(-1));
            winner_ = (w == 0 || w == 1) ? w : -1;
            endedAt_ = now;
        } else if (event == "MatchDestroyed") {
            inMatch_ = false;
            endedAt_ = 0;
        } else {
            ++unknown_;
        }
        return true;
    }

    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && inMatch_; }

    fx::Params Current(uint64_t now) const {
        using fx::Kind;
        if (endedAt_ && now - endedAt_ < 12000 && winner_ >= 0)
            return Make(Kind::Breathing, teamColor_[winner_], {}, 0.8);
        if (goalAt_ && now - goalAt_ < 2500) return Make(Kind::Strobe, teamColor_[goalTeam_], {}, 5);
        if (goalAt_ && now - goalAt_ < 6000) return Make(Kind::Comet, teamColor_[goalTeam_], {}, 1.2);
        return Make(Kind::Gradient, teamColor_[0], teamColor_[1], overtime_ ? 0.5 : 0.08);
    }

    int unknownEvents() const { return unknown_; }

private:
    static fx::Params Make(fx::Kind k, Rgb c1, Rgb c2 = {}, double speed = 0) {
        fx::Params p;
        p.kind = k;
        p.color1 = c1;
        p.color2 = c2;
        p.speed = speed;
        return p;
    }
    static bool ParseHex(const std::string& in, Rgb* out) {
        std::string s = in;
        if (!s.empty() && s[0] == '#') s = s.substr(1);
        if (s.size() != 6) return false;
        unsigned v = 0;
        for (char ch : s) {
            const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                        : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + static_cast<unsigned>(d);
        }
        *out = Rgb{static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
        return true;
    }

    uint64_t lastSeen_ = 0, goalAt_ = 0, endedAt_ = 0;
    bool inMatch_ = false, overtime_ = false;
    int goalTeam_ = 0, winner_ = -1, unknown_ = 0;
    Rgb teamColor_[2] = {kBlue, kOrange};
};

}  // namespace luma::app::games
