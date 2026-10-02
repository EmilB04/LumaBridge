// What LumaBridge knows about specific games: how (or whether) each one can drive the
// lights. Matched by exe name, or by name for games whose exe isn't known. Pure C++, tested.
// The hand-written profiles come first; the vendor-SDK games in sdk_games.h follow, made
// into profiles the same way.
#pragma once

#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "game_catalog.h"
#include "sdk_games.h"

namespace luma::app::games {

enum class ProfileKind {
    BuiltIn,    // LumaBridge reads the game's official data feed itself (no DLL in the game)
    VendorSdk,  // the game lights Logitech / Razer / ... gear through that vendor's SDK
    NoSupport,  // no known lighting support: Screen colors is the way
    NotAGame,   // a tool that looks like a game; ignored in Auto mode
};

enum class Feed { None, Cs2Gsi, RocketLeagueStats, WarThunderApi, Dota2Gsi, LeagueLiveClient, ForzaDataOut, FlightSimConnect, DcsExport, F1Telemetry,
            BeamNgOutGauge, DirtRallyUdp, Ams2Udp, XPlaneUdp, EliteStatus, AcSharedMemory, IRacingSdk, RaceRoomSharedMemory,
            WrcUdp };

struct GameProfile {
    const char* key;        // stable id ("cs2")
    const char* title;      // display name
    const char* exes[12];   // lower-case exe names (nullptr-terminated)
    const char* names[12];  // Normalize()d names to match when the exe is unknown
    ProfileKind kind;
    Feed feed;
    const char* how;        // one line: how it lights up
    const char* note;       // details / caveats
    // Its own lighting can't reach LumaBridge (an anti-cheat keeps LumaBridge's DLLs out): while
    // it runs, the devices it doesn't light itself show the screen's colors by default instead of
    // waiting for it (its vendor's gear, e.g. Logitech through G HUB, shows the game's own).
    bool blocked = false;
};

inline const GameProfile* HandWrittenProfiles(size_t* count) {
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
        {"forza", "Forza", {"forzahorizon5.exe", "forzahorizon4.exe", "forza_steamworks_release_final.exe", "forzamotorsport7.exe"},
         {"forzahorizon5", "forzahorizon4", "forzamotorsport", "forzamotorsport7"},
         ProfileKind::BuiltIn, Feed::ForzaDataOut, "Built in: the game's Data Out telemetry",
         "Rev lights: blue at low revs, then green, yellow and red as the engine climbs, and a red "
         "flash at the limiter. Switch on Data Out in the game's settings (its page says how)."},
        {"f1", "F1 2018 to F1 25",
         {"f1_25.exe", "f1_24.exe", "f1_23.exe", "f1_22.exe", "f1_2021_dx12.exe", "f1_2020_dx12.exe", "f1_2020.exe",
          "f1_2019_dx12.exe", "f1_2019.exe", "f1_2018.exe"},
         {"f125", "f124", "f123", "f122", "f12021", "f12020", "f12019", "f12018", "easportsf125", "easportsf124",
          "easportsf123"},
         ProfileKind::BuiltIn, Feed::F1Telemetry, "Built in: the game's UDP telemetry",
         "Rev lights from the shift lights: blue at low revs, then green, yellow and red as they climb, and a fast "
         "red flash once they're full. Switch on Telemetry in the game's settings (its page says how)."},
        {"beamng", "BeamNG.drive", {"beamng.drive.x64.exe", "beamng.drive.exe"}, {"beamngdrive"}, ProfileKind::BuiltIn,
         Feed::BeamNgOutGauge, "Built in: the game's OutGauge telemetry",
         "Rev lights from the engine speed (blue, green, yellow, red, and a red flash at the limit), and an amber "
         "pulse for the oil and battery warnings. Switch on OutGauge in the game's options (its page says how)."},
        {"lfs", "Live for Speed", {"lfs.exe"}, {"liveforspeed"}, ProfileKind::BuiltIn, Feed::BeamNgOutGauge,
         "Built in: the game's OutGauge telemetry",
         "Rev lights from the engine speed (blue, green, yellow, red, and a red flash on the shift light), and an "
         "amber pulse for the oil and battery warnings. Switch on OutGauge in the game's cfg.txt (its page says how)."},
        {"dirt", "DiRT Rally / DiRT 4", {"dirtrally2.exe", "dirtrally.exe", "dirt4.exe"},
         {"dirtrally20", "dirtrally2", "dirtrally", "dirt4"}, ProfileKind::BuiltIn, Feed::DirtRallyUdp,
         "Built in: the game's UDP telemetry",
         "Rev lights from the engine speed: blue at low revs, then green, yellow and red as it climbs. Switch on "
         "the telemetry in the game's settings file (its page says how)."},
        {"grid", "GRID / GRID Legends", {"gridlegends.exe"}, {"gridlegends", "grid", "grid2019"}, ProfileKind::BuiltIn,
         Feed::DirtRallyUdp, "Built in: the game's UDP telemetry",
         "Rev lights from the engine speed: blue at low revs, then green, yellow and red as it climbs. Switch on "
         "the telemetry in the game's settings file (its page says how)."},
        {"f1classic", "F1 2015 / F1 2016 / F1 2017", {"f1_2015.exe", "f1_2016.exe", "f1_2017.exe"},
         {"f12015", "f12016", "f12017"}, ProfileKind::BuiltIn, Feed::DirtRallyUdp, "Built in: the game's UDP telemetry",
         "Rev lights from the engine speed: blue at low revs, then green, yellow and red as it climbs. Switch on "
         "the telemetry in the game (its page says how)."},
        {"wrcg", "WRC Generations", {"wrcg.exe"}, {"wrcgenerations", "wrcgenerationsthefiawrcofficialgame"},
         ProfileKind::BuiltIn, Feed::DirtRallyUdp, "Built in: the game's UDP telemetry",
         "Rev lights from the engine speed: blue at low revs, then green, yellow and red as it climbs. Switch on "
         "the telemetry in the game's settings file (its page says how)."},
        {"wrc", "EA SPORTS WRC", {"wrc.exe"}, {"easportswrc"}, ProfileKind::BuiltIn, Feed::WrcUdp,
         "Built in: the game's UDP telemetry",
         "Rev lights from the engine speed on a stage: blue at low revs, then green, yellow and red as it climbs, "
         "and a red flash at the limiter. Set up once on its page (adds LumaBridge to the game's telemetry "
         "settings)."},
        {"ams2", "Automobilista 2 / Project CARS",
         {"ams2avx.exe", "ams2.exe", "pcars2avx.exe", "pcars2.exe", "pcars3.exe", "pcars64.exe", "pcars.exe"},
         {"automobilista2", "projectcars", "projectcars2", "projectcars3"}, ProfileKind::BuiltIn, Feed::Ams2Udp,
         "Built in: the game's UDP telemetry",
         "Rev lights from the engine speed (blue, green, yellow, red), a red flash at the limiter and an amber "
         "pulse for an engine warning. Switch on UDP in the game's options (its page says how)."},
        {"assettocorsa", "Assetto Corsa / Competizione / EVO", {"acs.exe", "ac2-win64-shipping.exe", "assettocorsaevo.exe"},
         {"assettocorsa", "assettocorsacompetizione", "assettocorsaevo", "assettocorsarally"}, ProfileKind::BuiltIn,
         Feed::AcSharedMemory,
         "Built in: the game's shared memory telemetry",
         "Rev lights from the engine speed (blue, green, yellow, red, and a red flash at the limit), and an amber "
         "pulse with the pit limiter on. Nothing to set up: the game publishes it for telemetry apps while you "
         "drive."},
        {"iracing", "iRacing", {"iracingsim64dx11.exe", "iracingsim64.exe"}, {"iracing"}, ProfileKind::BuiltIn,
         Feed::IRacingSdk, "Built in: the iRacing SDK (telemetry)",
         "Rev lights from your car's own shift lights, a red flash at the shift point, the yellow, blue and black "
         "flags, and an amber pulse with the pit limiter on. Nothing to set up: iRacing publishes it while you're "
         "in the car."},
        {"raceroom", "RaceRoom Racing Experience", {"rrre64.exe", "rrre.exe"}, {"raceroomracingexperience", "raceroom"},
         ProfileKind::BuiltIn, Feed::RaceRoomSharedMemory, "Built in: the game's shared memory telemetry",
         "Rev lights from the engine speed, a red flash at the shift point, and an amber pulse with the pit "
         "limiter on. Nothing to set up: the game publishes it for telemetry apps while you drive."},
        {"xplane", "X-Plane", {"x-plane.exe"}, {"xplane", "xplane11", "xplane12"}, ProfileKind::BuiltIn,
         Feed::XPlaneUdp, "Built in: the sim's UDP data output",
         "Blue in flight, amber then red as the G-load climbs, magenta under negative G, a slow dim blue while "
         "parked. Switch on the data output in the sim (its page says how)."},
        {"elite", "Elite Dangerous", {"elitedangerous64.exe"}, {"elitedangerous"}, ProfileKind::BuiltIn,
         Feed::EliteStatus, "Built in: the game's Status.json",
         "Your ship's state: the cockpit's orange in normal space, cyan in supercruise, red with hardpoints out, "
         "an FSD charge and jump, low fuel, danger, being interdicted and overheating. Nothing to set up."},
        {"msfs", "Microsoft Flight Simulator", {"flightsimulator.exe", "flightsimulator2024.exe"},
         {"microsoftflightsimulator", "microsoftflightsimulator2020", "microsoftflightsimulator2024"},
         ProfileKind::BuiltIn, Feed::FlightSimConnect, "Built in: SimConnect, the sim's own add-on interface",
         "The sky's color by time of day, your aircraft's beacon and strobe lights flashing, stall and overspeed "
         "warnings, low fuel and crashes. Needs SimConnect.dll from Microsoft's free Flight Simulator SDK (its "
         "page says how)."},
        {"dcs", "DCS World", {"dcs.exe"}, {"dcsworld", "dcsworldsteamedition"}, ProfileKind::BuiltIn, Feed::DcsExport,
         "Built in: DCS's export script (Export.lua)",
         "The sky's color by the mission's time of day, three greens with the gear down, red as the G climbs, "
         "and the master warning. Set up once on its page (adds a script to Saved Games\\DCS\\Scripts)."},
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
         "Logitech LIGHTSYNC through G HUB, Razer Chroma through Synapse",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC), and LumaBridge hands your Logitech gear "
         "to it while it runs; it lights Razer gear through Razer Synapse (Chroma) the same way. EA's anti-cheat "
         "keeps LumaBridge out of the game, so your other devices show your idle choice, or the screen's colors if "
         "you pick them on this page.", true},
        {"ets2", "Euro Truck Simulator 2", {"eurotrucks2.exe"}, {"eurotrucksimulator2"}, ProfileKind::VendorSdk,
         Feed::None, "Logitech LIGHTSYNC, Razer Chroma",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC). With Logitech LIGHTSYNC set up on the "
         "Integrations page, LumaBridge catches that lighting and shows it on every device; otherwise it hands "
         "your Logitech gear to the game while it runs. It also speaks Razer Chroma (install it on the "
         "Integrations page)."},
        {"tlou1", "The Last of Us Part I", {"tlou-i.exe", "tlou-i-l.exe"}, {"thelastofusparti", "thelastofuspart1"},
         ProfileKind::VendorSdk, Feed::None, "Logitech LIGHTSYNC",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC). With Logitech LIGHTSYNC set up on the "
         "Integrations page, LumaBridge catches that lighting and shows it on every device; otherwise it hands "
         "your Logitech gear to the game while it runs."},
        {"sims4", "The Sims 4", {"ts4_x64.exe"}, {"thesims4"}, ProfileKind::VendorSdk, Feed::None,
         "Logitech LIGHTSYNC, Razer Chroma, SteelSeries GameSense",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC). With Logitech LIGHTSYNC set up on the "
         "Integrations page, LumaBridge catches that lighting and shows it on every device; otherwise it hands "
         "your Logitech gear to the game while it runs. It also speaks Razer Chroma (install it on the "
         "Integrations page) and SteelSeries GameSense (nothing to set up)."},
        {"civ6", "Civilization VI", {"civilizationvi.exe", "civ6.exe"}, {"civilizationvi", "sidmeierscivilizationvi"},
         ProfileKind::VendorSdk, Feed::None, "Logitech LIGHTSYNC",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC). With Logitech LIGHTSYNC set up on the "
         "Integrations page, LumaBridge catches that lighting and shows it on every device; otherwise it hands "
         "your Logitech gear to the game while it runs. Which events drive which colors isn't publicly documented."},
        {"totalwarwh3", "Total War: WARHAMMER III", {"warhammer3.exe"}, {"totalwarwarhammeriii", "totalwarwarhammer3"},
         ProfileKind::VendorSdk, Feed::None, "Logitech LIGHTSYNC",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC). With Logitech LIGHTSYNC set up on the "
         "Integrations page, LumaBridge catches that lighting and shows it on every device; otherwise it hands "
         "your Logitech gear to the game while it runs. Which events drive which colors isn't publicly documented."},
        {"terraria", "Terraria", {"terraria.exe"}, {"terraria"}, ProfileKind::VendorSdk, Feed::None,
         "Logitech LIGHTSYNC",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC). With Logitech LIGHTSYNC set up on the "
         "Integrations page, LumaBridge catches that lighting and shows it on every device; otherwise it hands "
         "your Logitech gear to the game while it runs. Which events drive which colors isn't publicly documented."},
        {"gtav", "Grand Theft Auto V", {"gta5.exe"}, {"grandtheftautov", "gtav"}, ProfileKind::VendorSdk, Feed::None,
         "Logitech LIGHTSYNC",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC). With Logitech LIGHTSYNC set up on the "
         "Integrations page, LumaBridge catches that lighting and shows it on every device; otherwise it hands "
         "your Logitech gear to the game while it runs. Which events drive which colors isn't publicly documented; "
         "Rockstar's own anti-tamper is untested against LumaBridge."},
        {"factorio", "Factorio", {"factorio.exe"}, {"factorio"}, ProfileKind::VendorSdk, Feed::None,
         "Logitech LIGHTSYNC, Razer Chroma",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC). With Logitech LIGHTSYNC set up on the "
         "Integrations page, LumaBridge catches that lighting and shows it on every device; otherwise it hands "
         "your Logitech gear to the game while it runs. It also speaks Razer Chroma (install it on the "
         "Integrations page)."},
        {"ats", "American Truck Simulator", {"amtrucks.exe"}, {"americantrucksimulator"}, ProfileKind::VendorSdk,
         Feed::None, "Logitech LIGHTSYNC, Razer Chroma",
         "The game lights Logitech gear itself through G HUB (LIGHTSYNC). With Logitech LIGHTSYNC set up on the "
         "Integrations page, LumaBridge catches that lighting and shows it on every device; otherwise it hands "
         "your Logitech gear to the game while it runs. It also speaks Razer Chroma (install it on the "
         "Integrations page)."},
        {"apex", "Apex Legends", {"r5apex.exe", "r5apex_dx12.exe"}, {"apexlegends"}, ProfileKind::VendorSdk, Feed::None,
         "Razer Chroma",
         "Install Razer Chroma on the Integrations page, and switch the game's Razer Chroma option on if it has "
         "one. Its anti-cheat may refuse LumaBridge's DLL; if nothing happens, use Screen colors."},
        {"pubg", "PUBG: BATTLEGROUNDS", {"tslgame.exe"}, {"pubgbattlegrounds", "playerunknownsbattlegrounds"},
         ProfileKind::VendorSdk, Feed::None, "Razer Chroma",
         "Install Razer Chroma on the Integrations page, and switch the game's Razer Chroma option on if it has "
         "one. Its anti-cheat may refuse LumaBridge's DLL; if nothing happens, use Screen colors."},
        {"marvelrivals", "Marvel Rivals", {"marvel-win64-shipping.exe"}, {"marvelrivals"}, ProfileKind::VendorSdk,
         Feed::None, "Razer Chroma (switch on in its launcher)",
         "Turn on Razer Chroma RGB in the Marvel Rivals launcher's settings (off by default). It talks to Razer's "
         "own Chroma app, so it lights Razer gear through Razer Synapse; your other devices show your idle choice, "
         "or the screen's colors if you pick them on this page."},
        {"hitman3", "HITMAN World of Assassination", {"hitman3.exe"}, {"hitmanworldofassassination", "hitman3"},
         ProfileKind::VendorSdk, Feed::None, "Razer Chroma",
         "Install Razer Chroma on the Integrations page and switch on Razer Chroma in the game's options. If the "
         "game crashes at start with it on, switch it off there and use Screen colors."},
        {"destiny2", "Destiny 2", {"destiny2.exe"}, {"destiny2"}, ProfileKind::NoSupport, Feed::None,
         "No known lighting support", "No dynamic lighting of its own (only fixed profiles made by fans). Use Screen colors."},
        {"r6siege", "Rainbow Six Siege", {"rainbowsix.exe", "rainbowsix_vulkan.exe"},
         {"tomclancysrainbowsixsiege", "rainbowsixsiege", "tomclancysrainbowsixsiegex"}, ProfileKind::NoSupport, Feed::None,
         "No known lighting support", "No dynamic lighting of its own (only fixed profiles made by fans). Use Screen colors."},
        {"valorant", "VALORANT", {"valorant-win64-shipping.exe"}, {"valorant"}, ProfileKind::NoSupport, Feed::None,
         "No known lighting support",
         "No lighting of its own and no game data on this PC (only fixed iCUE / Synapse profiles), and Riot's "
         "Vanguard anti-cheat keeps everything else out. Use Screen colors."},
        {"rdr2", "Red Dead Redemption 2", {"rdr2.exe"}, {"reddeadredemption2"}, ProfileKind::NoSupport, Feed::None,
         "No known lighting support", "No dynamic lighting of its own (only fixed profiles made by fans). Use Screen colors."},
        {"cod", "Call of Duty", {"cod.exe"}, {"callofduty"}, ProfileKind::NoSupport, Feed::None,
         "No known lighting support",
         "Only fixed iCUE / Chroma profiles in the game's colors, nothing that follows the game. Use Screen colors."},
        {"wallpaperengine", "Wallpaper Engine", {"wallpaper32.exe", "wallpaper64.exe"}, {"wallpaperengine"},
         ProfileKind::NotAGame, Feed::None, "Not a game (animated wallpapers)", "Ignored in Auto mode."},
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

// "Razer Chroma, Logitech LIGHTSYNC" for a game's SdkBits.
inline std::string SdkNames(unsigned sdks) {
    static const std::pair<unsigned, const char*> kNames[] = {{kChroma, "Razer Chroma"},
                                                              {kLightsync, "Logitech LIGHTSYNC"},
                                                              {kIcue, "Corsair iCUE"},
                                                              {kGameSense, "SteelSeries GameSense"},
                                                              {kAlienFx, "Alienware AlienFX"},
                                                              {kAura, "ASUS Aura Sync"}};
    std::string out;
    for (const auto& [bit, name] : kNames)
        if (sdks & bit) out += (out.empty() ? "" : ", ") + std::string(name);
    return out;
}

// What a vendor-SDK game needs, one sentence per SDK it speaks.
inline std::string SdkNote(unsigned sdks) {
    std::string out;
    auto add = [&](const char* s) { out += (out.empty() ? "" : " ") + std::string(s); };
    if (sdks & kChroma)
        add("Razer Chroma: install it on the Integrations page, and switch Chroma on in the game's options if it "
            "has one.");
    if (sdks & kLightsync)
        add("Logitech LIGHTSYNC: with it set up on the Integrations page LumaBridge catches the game's lighting and "
            "shows it on every device; otherwise it hands your Logitech gear to the game while it runs.");
    if (sdks & kIcue)
        add("Corsair iCUE: on the Integrations page, Corsair > Add to a game, and pick this game's folder.");
    if (sdks & kGameSense) add("SteelSeries GameSense: nothing to set up, LumaBridge answers it while it runs.");
    if (sdks & kAlienFx) add("Alienware AlienFX: install it on the Integrations page.");
    if (sdks & kAura)
        add("ASUS Aura Sync: the game lights your ASUS Aura gear itself, through Armoury Crate's Aura service, which "
            "LumaBridge can't see. If the lights flicker between the game and LumaBridge, pause LumaBridge while you "
            "play.");
    if (sdks & ~unsigned(kAura))
        add("If nothing lights up (an anti-cheat can keep LumaBridge's files out), use Screen colors.");
    return out;
}

namespace detail {

// Every profile, with lookups by exe, name and key. Built once; the profiles never move after.
struct ProfileIndex {
    std::vector<GameProfile> all;
    std::unordered_map<std::string, const GameProfile*> byExe, byName, byKey;
};

inline const ProfileIndex& Index() {
    static const ProfileIndex index = [] {
        ProfileIndex ix;
        static std::deque<std::string> text;  // the made profiles' strings (a deque never moves them)
        auto keep = [](std::string s) { return text.emplace_back(std::move(s)).c_str(); };
        size_t count = 0;
        const GameProfile* hand = HandWrittenProfiles(&count);
        ix.all.assign(hand, hand + count);
        // Names and exes the hand-written profiles already claim: those games aren't repeated.
        std::unordered_set<std::string> taken;
        for (const GameProfile& p : ix.all) {
            for (const char* x : p.exes)
                if (x) taken.insert(x);
            for (const char* x : p.names)
                if (x) taken.insert(x);
        }
        size_t sdkCount = 0;
        const SdkGame* sdk = SdkGames(&sdkCount);
        for (size_t i = 0; i < sdkCount; ++i) {
            const SdkGame& g = sdk[i];
            const std::string name = Normalize(g.title);
            const std::string alias = g.alias ? Normalize(g.alias) : std::string();
            if (taken.count(name) || (g.exe && taken.count(g.exe)) || (!alias.empty() && taken.count(alias))) continue;
            GameProfile p{};
            p.key = keep("sdk-" + name);
            p.title = g.title;
            p.exes[0] = g.exe;
            p.exes[1] = g.exe ? g.exe2 : nullptr;
            p.names[0] = keep(name);
            if (!alias.empty() && alias != name) p.names[1] = keep(alias);
            p.kind = ProfileKind::VendorSdk;
            p.feed = Feed::None;
            p.how = keep(SdkNames(g.sdks));
            p.note = keep(SdkNote(g.sdks));
            ix.all.push_back(p);
            taken.insert(name);
        }
        // The first profile to claim an exe or a name keeps it.
        for (const GameProfile& p : ix.all) {
            for (const char* x : p.exes)
                if (x) ix.byExe.emplace(x, &p);
            for (const char* x : p.names)
                if (x) ix.byName.emplace(x, &p);
            ix.byKey.emplace(p.key, &p);
        }
        return ix;
    }();
    return index;
}

}  // namespace detail

inline const GameProfile* Profiles(size_t* count) {
    const auto& ix = detail::Index();
    *count = ix.all.size();
    return ix.all.data();
}

// `exe`: the exe file name (any case), `name`: display name. nullptr when unknown.
inline const GameProfile* FindProfile(const std::string& exe, const std::string& name) {
    const auto& ix = detail::Index();
    std::string e = exe;
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!e.empty())
        if (auto it = ix.byExe.find(e); it != ix.byExe.end()) return it->second;
    const std::string n = Normalize(name);
    if (!n.empty())
        if (auto it = ix.byName.find(n); it != ix.byName.end()) return it->second;
    return nullptr;
}

inline const GameProfile* ProfileByKey(const char* key) {
    const auto& ix = detail::Index();
    auto it = ix.byKey.find(key ? key : "");
    return it == ix.byKey.end() ? nullptr : it->second;
}

}  // namespace luma::app::games
