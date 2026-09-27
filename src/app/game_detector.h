// Finds running games, whether or not they do any lighting (see game_catalog.h for what
// counts as a game), and checks which of them loaded a lighting SDK. UI-thread only; Poll()
// is cheap between scans.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace luma::app {

struct RunningGame {
    uint32_t pid = 0;
    std::string name;    // display name ("Rocket League")
    std::string exe;     // "RocketLeague.exe"
    std::string folder;  // its folder in the store's library, if any
    std::string store;   // "Steam", "Epic Games", ... or "Windows" (Game Bar list)
    std::string sdk;     // lighting SDK it loaded ("Razer Chroma"), empty if none seen
    bool modulesReadable = false;  // false: didn't / couldn't look inside (anti-cheat, elevated game)
    bool antiCheat = false;        // anti-cheat found with the game: never looked inside
};

class GameDetector {
public:
    // Rescans every couple of seconds; returns true when the list changed.
    bool Poll(uint64_t now);
    const std::vector<RunningGame>& Games() const { return games_; }

private:
    struct Entry {
        std::wstring exeName;  // to notice pid reuse
        bool candidate = false;  // a game by its path; shown once it has a window
        bool hadWindow = false;
        RunningGame info;
        uint64_t nextModuleCheck = 0;
    };

    void LoadGameConfigStore();
    void Inspect(uint32_t pid, Entry* e);
    void CheckModules(Entry* e, uint64_t now);

    std::map<uint32_t, Entry> entries_;
    std::set<std::wstring> windowsGames_;  // lower-case exe paths Windows lists as games
    std::vector<RunningGame> games_;
    uint64_t nextScan_ = 0;
    uint64_t nextStoreLoad_ = 0;
    bool antiCheatRunning_ = false;  // an anti-cheat process is running right now
};

}  // namespace luma::app
