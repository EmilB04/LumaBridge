// What LumaBridge knows about specific games: how (or whether) each one can drive the
// lights. Matched by exe name, or by name for games whose exe isn't known. Pure C++, tested.
#pragma once

#include <cstring>
#include <string>

#include "game_catalog.h"

namespace luma::app::games {

enum class ProfileKind {
    BuiltIn,    // LumaBridge reads the game's official data feed itself (no DLL in the game)
    VendorSdk,  // the game lights Logitech / Razer / ... gear through that vendor's SDK
    NoSupport,  // no known lighting support: Screen colors is the way
    NotAGame,   // a tool that looks like a game; ignored in Auto mode
};

enum class Feed { None, Cs2Gsi, RocketLeagueStats, WarThunderApi, Dota2Gsi, LeagueLiveClient, ForzaDataOut };

struct GameProfile {
    const char* key;        // stable id ("cs2")
    const char* title;      // display name
    const char* exes[3];    // lower-case exe names (nullptr-terminated)
    const char* names[3];   // Normalize()d names to match when the exe is unknown
    ProfileKind kind;
    Feed feed;
    const char* how;        // one line: how it lights up
    const char* note;       // details / caveats
    // Its own lighting can't reach LumaBridge (an anti-cheat keeps LumaBridge's DLLs out): while
    // it runs, the devices it doesn't light itself show the screen's colors by default instead of
    // waiting for it (its vendor's gear, e.g. Logitech through G HUB, shows the game's own).
    bool blocked = false;
};

inline const GameProfile* Profiles(size_t* count) {
    static const GameProfile kProfiles[] = {
        {"cs2", "Counter-Strike 2", {"cs2.exe"}, {"counterstrike2"}, ProfileKind::BuiltIn, Feed::Cs2Gsi,
         "Built in: Valve's Game State Integration",
         "Team colors, freeze time, low health, flashbangs, fire, the bomb (faster as it ticks), "
         "kills, headshots and round results. Set up once on its page in the Games List, then restart CS2."},
        {"rocketleague", "Rocket League", {"rocketleague.exe"}, {"rocketleague"}, ProfileKind::BuiltIn,
         Feed::RocketLeagueStats, "Built in: Psyonix's Stats API",
         "Your team's color around the fans, a burst in the scoring team's color on every goal, "
         "overtime and the winner. Switch the Stats API on once on its page in the Games List, then "
         "restart Rocket League."},
        {"warthunder", "War Thunder", {"aces.exe"}, {"warthunder"}, ProfileKind::BuiltIn, Feed::WarThunderApi,
         "Built in: the game's local status page",
         "Tanks: crew health from green to red, with a flash when a crew member is lost. Aircraft: "
         "war emergency power and low fuel. Nothing to set up."},
        {"dota2", "Dota 2", {"dota2.exe"}, {"dota2"}, ProfileKind::BuiltIn, Feed::Dota2Gsi,
         "Built in: Valve's Game State Integration",
         "Your team's color (dimmer at night), low health, stuns and hexes, kills, death, and victory or "
         "defeat. Set up once on its page in the Games List, add -gamestateintegration to Dota 2's launch "
         "options in Steam, then restart Dota 2."},
        {"league", "League of Legends", {"league of legends.exe"}, {"leagueoflegends"}, ProfileKind::BuiltIn,
         Feed::LeagueLiveClient, "Built in: Riot's Live Client Data API",
         "Your side's color, low health, death, your kills and multikills, your team's dragons (in the "
         "dragon's color), barons and heralds, and victory or defeat. Nothing to set up."},
        {"forza", "Forza", {"forzahorizon5.exe", "forzahorizon4.exe"}, {"forzahorizon5", "forzahorizon4", "forzamotorsport"},
         ProfileKind::BuiltIn, Feed::ForzaDataOut, "Built in: the game's Data Out telemetry",
         "Rev lights: blue at low revs, then green, yellow and red as the engine climbs, and a red "
         "flash at the limiter. Switch on Data Out in the game's settings (its page says how)."},
        {"overwatch", "Overwatch 2", {"overwatch.exe"}, {"overwatch", "overwatch2"}, ProfileKind::VendorSdk,
         Feed::None, "Razer Chroma (hero effects)",
         "Install Razer Chroma on the Integrations page. Blizzard's anti-cheat may refuse an "
         "unsigned DLL; if nothing happens, use Screen colors."},
        {"bf1", "Battlefield 1", {"bf1.exe"}, {"battlefield1"}, ProfileKind::VendorSdk, Feed::None,
         "Logitech LIGHTSYNC through G HUB",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC), and LumaBridge hands your Logitech gear "
         "to it while it runs. EA's anti-cheat keeps LumaBridge out of the game, so your other devices show your "
         "idle choice, or the screen's colors if you pick them on this page.", true},
        {"bf2042", "Battlefield 2042", {"bf2042.exe"}, {"battlefield2042"}, ProfileKind::VendorSdk, Feed::None,
         "Logitech LIGHTSYNC through G HUB",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC), and LumaBridge hands your Logitech gear "
         "to it while it runs. EA's anti-cheat keeps LumaBridge out of the game, so your other devices show your "
         "idle choice, or the screen's colors if you pick them on this page.", true},
        {"bfv", "Battlefield V", {"bfv.exe"}, {"battlefieldv", "battlefield5"}, ProfileKind::VendorSdk, Feed::None,
         "Logitech LIGHTSYNC through G HUB",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC), and LumaBridge hands your Logitech gear "
         "to it while it runs. EA's anti-cheat keeps LumaBridge out of the game, so your other devices show your "
         "idle choice, or the screen's colors if you pick them on this page.", true},
        {"bf6", "Battlefield 6", {"bf6.exe"}, {"battlefield6"}, ProfileKind::VendorSdk, Feed::None,
         "Logitech LIGHTSYNC through G HUB",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC), and LumaBridge hands your Logitech gear "
         "to it while it runs. EA's anti-cheat keeps LumaBridge out of the game, so your other devices show your "
         "idle choice, or the screen's colors if you pick them on this page.", true},
        {"bombanana", "BOMBANANA!", {nullptr}, {"bombanana"}, ProfileKind::NoSupport, Feed::None,
         "No known lighting support", "Use Screen colors."},
        {"nobackup", "NO BACKUP", {nullptr}, {"nobackup"}, ProfileKind::NoSupport, Feed::None,
         "No known lighting support", "Use Screen colors."},
        {"rvthereyet", "RV There Yet?", {nullptr}, {"rvthereyet"}, ProfileKind::NoSupport, Feed::None,
         "No known lighting support", "Use Screen colors."},
        {"dsx", "DSX", {"dsx.exe"}, {"dsx"}, ProfileKind::NotAGame, Feed::None,
         "Not a game (DualSense controller utility)", "Ignored in Auto mode."},
    };
    *count = sizeof kProfiles / sizeof kProfiles[0];
    return kProfiles;
}

// `exe`: the exe file name (any case), `name`: display name. nullptr when unknown.
inline const GameProfile* FindProfile(const std::string& exe, const std::string& name) {
    std::string e = exe;
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string n = Normalize(name);
    size_t count = 0;
    const GameProfile* all = Profiles(&count);
    for (size_t i = 0; i < count; ++i)
        for (const char* x : all[i].exes)
            if (x && e == x) return &all[i];
    for (size_t i = 0; i < count; ++i)
        for (const char* x : all[i].names)
            if (x && !n.empty() && n == x) return &all[i];
    return nullptr;
}

inline const GameProfile* ProfileByKey(const char* key) {
    size_t count = 0;
    const GameProfile* all = Profiles(&count);
    for (size_t i = 0; i < count; ++i)
        if (std::strcmp(all[i].key, key) == 0) return &all[i];
    return nullptr;
}

}  // namespace luma::app::games
