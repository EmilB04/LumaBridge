// Built-in game feeds: official data games publish on this PC, turned into lighting without
// any DLL in the game (so anti-cheat has nothing to object to).
//   Counter-Strike 2  Valve Game State Integration: CS2 POSTs JSON to 127.0.0.1:kCs2Port
//                     (set up by the cfg file WriteCs2Config() writes).
//   Rocket League     Psyonix Stats API: JSON events on a local port once enabled in
//                     TAGame\Config\DefaultStatsAPI.ini (EnableRocketLeagueStats()).
//   War Thunder       the game's local status page, http://127.0.0.1:8111 (always on).
//   Dota 2            Valve Game State Integration, like CS2 (same port, told apart by app id).
//   League of Legends Riot's Live Client Data API, https://127.0.0.1:2999 (always on in a match).
//   Forza             the games' "Data Out" UDP telemetry, to 127.0.0.1:forzaPort (switched on
//                     in the game's settings).
//   F1 24 / F1 25     the games' own UDP telemetry, to 127.0.0.1:f1Port (switched on in the
//                     game's settings), reading only the Car Telemetry packet.
//   Flight Simulator  SimConnect, the sim's own add-on interface (SimConnect.dll from Microsoft's
//                     free Flight Simulator SDK, or next to LumaBridge.exe).
//   DCS World         LumaBridge.lua, loaded from DCS's Export.lua, sends UDP to 127.0.0.1:49717.
// The polled / listened feeds are only attempted while their games run.
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cs2_lighting.h"
#include "dcs_lighting.h"
#include "dota2_lighting.h"
#include "f1_lighting.h"
#include "flight_sim_lighting.h"
#include "forza_lighting.h"
#include "league_lighting.h"
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
    void SetForzaPort(int port) { forzaPort_ = port; }
    int forzaPort() const { return forzaPort_; }
    void SetF1Port(int port) { f1Port_ = port; }
    int f1Port() const { return f1Port_; }
    // Which of the polled games are running (from the game detector).
    struct Running {
        bool rocketLeague = false, warThunder = false, league = false, forza = false, flightSim = false, dcs = false,
             f1 = false;
    };
    void SetRunning(const Running& r) {
        rlRunning_ = r.rocketLeague;
        wtRunning_ = r.warThunder;
        leagueRunning_ = r.league;
        forzaRunning_ = r.forza;
        msfsRunning_ = r.flightSim;
        dcsRunning_ = r.dcs;
        f1Running_ = r.f1;
    }

    struct Feed {
        bool active = false;  // in a match / vehicle and sending
        fx::Params effect;
    };
    Feed Cs2(uint64_t now);
    Feed RocketLeague(uint64_t now);
    Feed WarThunder(uint64_t now);
    Feed Dota2(uint64_t now);
    Feed League(uint64_t now);
    Feed Forza(uint64_t now);
    Feed F1(uint64_t now);
    Feed FlightSim(uint64_t now);
    Feed Dcs(uint64_t now);

    bool Cs2Listening() const { return cs2Listen_ != INVALID_SOCKET; }
    bool Cs2Seen() const { return cs2Seen_; }            // CS2 has sent at least once
    bool RocketLeagueConnected() const { return rlConnected_; }
    bool WarThunderSeen() const { return wtSeen_; }
    bool Dota2Seen() const { return dotaSeen_; }
    bool LeagueSeen() const { return leagueSeen_; }
    bool ForzaSeen() const { return forzaSeen_; }
    bool ForzaPortBusy() const { return forzaBusy_; }
    bool F1Seen() const { return f1Seen_; }
    bool F1PortBusy() const { return f1Busy_; }
    bool FlightSimSeen() const { return msfsSeen_; }
    // SimConnect.dll: 1 found, 0 not found, -1 not looked yet (the sim hasn't run).
    int FlightSimDll() const { return msfsDll_; }
    bool FlightSimConnected() const { return msfsConnected_; }
    bool DcsSeen() const { return dcsSeen_; }
    bool DcsPortBusy() const { return dcsBusy_; }

private:
    void Cs2Accept();
    void Cs2Serve(SOCKET s);
    void RocketLeagueLoop();
    void WarThunderLoop();
    void LeagueLoop();
    void ForzaLoop();
    void F1Loop();
    void FlightSimLoop();
    void DcsLoop();
    void RlHandle(std::string* buffer);

    std::atomic<bool> stop_{false};
    bool wsa_ = false;
    SOCKET cs2Listen_ = INVALID_SOCKET;
    std::thread cs2Thread_, rlThread_, wtThread_, leagueThread_, forzaThread_, f1Thread_, msfsThread_, dcsThread_;
    std::atomic<bool> rlRunning_{false}, wtRunning_{false}, leagueRunning_{false}, forzaRunning_{false},
        msfsRunning_{false}, dcsRunning_{false}, f1Running_{false};
    std::atomic<int> rlPort_{49123}, forzaPort_{games::ForzaLighting::kDefaultPort}, f1Port_{games::F1Lighting::kDefaultPort};
    std::atomic<bool> cs2Seen_{false}, rlConnected_{false}, wtSeen_{false}, dotaSeen_{false}, leagueSeen_{false},
        forzaSeen_{false}, forzaBusy_{false}, msfsSeen_{false}, msfsConnected_{false}, dcsSeen_{false}, dcsBusy_{false},
        f1Seen_{false}, f1Busy_{false};
    std::atomic<int> msfsDll_{-1};

    std::mutex mutex_;  // guards the engines
    games::Cs2Lighting cs2_;
    games::RocketLeagueLighting rl_;
    int rlLoggedTeam_ = -1, rlUpdatesWithoutTeam_ = 0;  // for the log (RlHandle)
    games::WarThunderLighting wt_;
    games::Dota2Lighting dota_;
    games::LeagueLighting league_;
    games::ForzaLighting forza_;
    games::F1Lighting f1_;
    games::FlightSimLighting msfs_;
    games::DcsLighting dcs_;
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

// Dota 2: the same, in game\dota\cfg\gamestate_integration (the folder is made if missing).
std::wstring Dota2ConfigPath(const std::wstring& gameDir);
bool Dota2ConfigInstalled(const std::wstring& gameDir);
std::string Dota2ConfigText();

// DCS World: its Saved Games folders (Saved Games\DCS, DCS.openbeta, ...) that exist.
std::vector<std::wstring> DcsSavedGames();
// Whether Export.lua in any of them loads LumaBridge.lua.
bool DcsSetUp();
// Adds (or with `remove`, takes out) LumaBridge.lua and its line in Export.lua, in every DCS
// Saved Games folder. "" on success, else what went wrong.
std::string DcsSetUpScripts(bool remove);

// Writes `text` to `path` (UTF-8, no BOM). False with GetLastError() set on failure
// (ERROR_ACCESS_DENIED under Program Files: the caller then retries elevated).
bool WriteTextFile(const std::wstring& path, const std::string& text);

}  // namespace luma::app
