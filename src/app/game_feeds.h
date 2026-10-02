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
//   F1 2018 to F1 25  the games' own UDP telemetry, to 127.0.0.1:f1Port (switched on in the
//                     game's settings), reading only the Car Telemetry packet.
//   BeamNG.drive      OutGauge UDP to 127.0.0.1:beamngPort (Live for Speed too); DiRT Rally,
//                     DiRT 4, GRID, F1 2015-2017 and WRC Generations (Codemasters' format),
//                     Automobilista 2 / Project CARS and EA SPORTS WRC: their UDP telemetry;
//                     X-Plane: its UDP data output; all only while their game runs.
//   Assetto Corsa,    the shared memory blocks these games publish for telemetry apps
//   iRacing, RaceRoom (opened read-only by name, while the game runs).
//   Elite Dangerous   its Status.json in Saved Games (always written by the game).
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

#include "ac_lighting.h"
#include "ams2_lighting.h"
#include "beamng_lighting.h"
#include "cs2_lighting.h"
#include "dirt_lighting.h"
#include "dcs_lighting.h"
#include "dota2_lighting.h"
#include "elite_lighting.h"
#include "f1_lighting.h"
#include "flight_sim_lighting.h"
#include "forza_lighting.h"
#include "iracing_lighting.h"
#include "league_lighting.h"
#include "effects.h"
#include "raceroom_lighting.h"
#include "rocket_league_lighting.h"
#include "war_thunder_lighting.h"
#include "wrc_lighting.h"
#include "xplane_lighting.h"

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
    // The racing and flight games' UDP feeds, one generic receiver each (UdpLoop).
    enum Udp { kBeamNg, kDirt, kAms2, kXPlane, kWrc, kUdpCount };
    void SetUdpPort(Udp u, int port) { udp_[u].port = port; }
    int UdpPort(Udp u) const { return udp_[u].port; }
    bool UdpSeen(Udp u) const { return udp_[u].seen; }
    bool UdpPortBusy(Udp u) const { return udp_[u].busy; }
    bool EliteSeen() const { return eliteSeen_; }
    // The racing games' shared memory blocks (Assetto Corsa, iRacing, RaceRoom).
    enum Memory { kAc, kIRacing, kRaceRoom, kMemoryCount };
    bool MemorySeen(Memory m) const { return memory_[m].seen; }    // a car on track this session
    bool MemoryFound(Memory m) const { return memory_[m].found; }  // the game's block is open now
    // Which of the polled games are running (from the game detector).
    struct Running {
        bool rocketLeague = false, warThunder = false, league = false, forza = false, flightSim = false, dcs = false,
             f1 = false, beamng = false, dirt = false, ams2 = false, xplane = false, elite = false, wrc = false,
             ac = false, iracing = false, raceroom = false;
    };
    void SetRunning(const Running& r) {
        rlRunning_ = r.rocketLeague;
        wtRunning_ = r.warThunder;
        leagueRunning_ = r.league;
        forzaRunning_ = r.forza;
        msfsRunning_ = r.flightSim;
        dcsRunning_ = r.dcs;
        f1Running_ = r.f1;
        udp_[kBeamNg].running = r.beamng;
        udp_[kDirt].running = r.dirt;
        udp_[kAms2].running = r.ams2;
        udp_[kXPlane].running = r.xplane;
        udp_[kWrc].running = r.wrc;
        memory_[kAc].running = r.ac;
        memory_[kIRacing].running = r.iracing;
        memory_[kRaceRoom].running = r.raceroom;
        eliteRunning_ = r.elite;
    }

    struct Feed {
        bool active = false;  // in a match / vehicle and sending
        fx::Params effect;
    };
    Feed Cs2(uint64_t now);
    games::BombCountdown Cs2Bomb(uint64_t now, int fuseSeconds = 40) const;
    Feed RocketLeague(uint64_t now);
    Feed WarThunder(uint64_t now);
    Feed Dota2(uint64_t now);
    Feed League(uint64_t now);
    Feed Forza(uint64_t now);
    Feed F1(uint64_t now);
    Feed FlightSim(uint64_t now);
    Feed Dcs(uint64_t now);
    Feed BeamNg(uint64_t now);
    Feed Dirt(uint64_t now);
    Feed Ams2(uint64_t now);
    Feed XPlane(uint64_t now);
    Feed Elite(uint64_t now);
    Feed Wrc(uint64_t now);
    Feed Ac(uint64_t now);
    Feed IRacing(uint64_t now);
    Feed RaceRoom(uint64_t now);

    bool Cs2Listening() const { return cs2Listen_ != INVALID_SOCKET; }
    bool Cs2Receiving(uint64_t now) const;
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
    void UdpLoop(Udp u);
    void EliteLoop();
    void MemoryLoop();
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

    struct UdpGame {
        std::atomic<bool> running{false}, seen{false}, busy{false};
        std::atomic<int> port{0};
        std::thread thread;
    };
    UdpGame udp_[kUdpCount];
    std::thread eliteThread_;
    std::atomic<bool> eliteRunning_{false}, eliteSeen_{false};
    struct MemoryGame {
        std::atomic<bool> running{false}, seen{false}, found{false};
    };
    MemoryGame memory_[kMemoryCount];
    std::thread memoryThread_;

    mutable std::mutex mutex_;  // guards the engines
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
    games::BeamNgLighting beamng_;
    games::DirtLighting dirt_;
    games::Ams2Lighting ams2_;
    games::XPlaneLighting xplane_;
    games::EliteLighting elite_;
    games::WrcLighting wrc_;
    games::AcLighting ac_;
    games::IRacingLighting iracing_;
    games::RaceRoomLighting raceroom_;
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

// EA SPORTS WRC: Documents\My Games\WRC\telemetry (empty when the game hasn't made it yet), and
// whether its config.json sends LumaBridge's packets (and to which port: 0 when not).
std::wstring WrcTelemetryDir();
int WrcConfigPort();
// Adds (or with `remove`, takes out) lumabridge.json and LumaBridge's entry in config.json,
// sending to `port`. "" on success, else what went wrong.
std::string WrcSetUp(int port, bool remove);

// Writes `text` to `path` (UTF-8, no BOM). False with GetLastError() set on failure
// (ERROR_ACCESS_DENIED under Program Files: the caller then retries elevated).
bool WriteTextFile(const std::wstring& path, const std::string& text);

}  // namespace luma::app
