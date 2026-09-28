// Built-in game feeds: official data games publish on this PC, turned into lighting without
// any DLL in the game (so anti-cheat has nothing to object to).
//   Counter-Strike 2  Valve Game State Integration: CS2 POSTs JSON to 127.0.0.1:kCs2Port
//                     (set up by the cfg file WriteCs2Config() writes).
//   Rocket League     Psyonix Stats API: JSON events on a local port once enabled in
//                     TAGame\Config\DefaultStatsAPI.ini (EnableRocketLeagueStats()).
//   War Thunder       the game's local status page, http://127.0.0.1:8111 (always on).
// The Rocket League / War Thunder connections are only attempted while those games run.
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "cs2_lighting.h"
#include "effects.h"
#include "rocket_league_lighting.h"
#include "war_thunder_lighting.h"

namespace luma::app {

class GameFeeds {
public:
    static constexpr int kCs2Port = 49715;
    static constexpr const char* kCs2Token = "lumabridge";

    ~GameFeeds() { Stop(); }
    void Start();
    void Stop();

    void SetRocketLeaguePort(int port) { rlPort_ = port; }
    // Which of the polled games are running (from the game detector).
    void SetRunning(bool rocketLeague, bool warThunder) {
        rlRunning_ = rocketLeague;
        wtRunning_ = warThunder;
    }

    struct Feed {
        bool active = false;  // in a match / vehicle and sending
        fx::Params effect;
    };
    Feed Cs2(uint64_t now);
    Feed RocketLeague(uint64_t now);
    Feed WarThunder(uint64_t now);

    bool Cs2Listening() const { return cs2Listen_ != INVALID_SOCKET; }
    bool Cs2Seen() const { return cs2Seen_; }            // CS2 has sent at least once
    bool RocketLeagueConnected() const { return rlConnected_; }
    bool WarThunderSeen() const { return wtSeen_; }

private:
    void Cs2Accept();
    void Cs2Serve(SOCKET s);
    void RocketLeagueLoop();
    void WarThunderLoop();
    void RlHandle(std::string* buffer);

    std::atomic<bool> stop_{false};
    bool wsa_ = false;
    SOCKET cs2Listen_ = INVALID_SOCKET;
    std::thread cs2Thread_, rlThread_, wtThread_;
    std::atomic<bool> rlRunning_{false}, wtRunning_{false};
    std::atomic<int> rlPort_{49123};
    std::atomic<bool> cs2Seen_{false}, rlConnected_{false}, wtSeen_{false};

    std::mutex mutex_;  // guards the engines
    games::Cs2Lighting cs2_;
    games::RocketLeagueLighting rl_;
    int rlLoggedTeam_ = -1, rlUpdatesWithoutTeam_ = 0;  // for the log (RlHandle)
    games::WarThunderLighting wt_;
};

// Setup helpers (Integrations page). `gameDir` is the game's install folder. They return an
// empty string on success, else what went wrong.
std::wstring Cs2ConfigPath(const std::wstring& gameDir);
bool Cs2ConfigInstalled(const std::wstring& gameDir);

std::wstring RocketLeagueStatsIni(const std::wstring& gameDir);
bool RocketLeagueStatsEnabled(const std::wstring& gameDir);
int RocketLeagueStatsPort(const std::wstring& gameDir);  // 49123 unless the ini says otherwise
// The ini text with the Stats API switched on / off ("" when the ini doesn't exist).
std::string RocketLeagueStatsText(const std::wstring& gameDir, bool enable);

// The text of the CS2 cfg file.
std::string Cs2ConfigText();

// Writes `text` to `path` (UTF-8, no BOM). False with GetLastError() set on failure
// (ERROR_ACCESS_DENIED under Program Files: the caller then retries elevated).
bool WriteTextFile(const std::wstring& path, const std::string& text);

}  // namespace luma::app
