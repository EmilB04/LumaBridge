// App preferences ([App] section of %LOCALAPPDATA%\LumaBridge\LumaBridge.ini) and the
// "start with Windows" registry entry. Shared settings (calibration, devices, ...) live in
// luma::Config so the game DLLs read the same values when the app isn't running.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "color.h"
#include "config.h"
#include "device_lighting.h"
#include "effects.h"

namespace luma::app {

enum class Mode { Auto, Manual };
// Manual effects are the per-LED effects (ColorCycle is the classic "rainbow": all LEDs one
// hue; RainbowWave spreads the hues around each fan).
using ManualEffect = fx::Kind;
// What Auto mode shows when no game is sending lighting. ArmouryCrate hands the lights back
// (stop + start Armoury Crate so it re-applies its effect).
enum class IdleBehavior { ManualColor, Rainbow, Off, ArmouryCrate };

// Per-game choice on the Games List page: what shows while the game runs without lighting of
// its own. Color: the game's own color (Prefs::gameColors).
enum class GameMode { Default, Screen, Idle, Color };

// Dashboard cards, in display order (ids; see ui.cpp). Cards not listed are hidden.
inline const std::vector<std::string>& DefaultDashboard() {
    static const std::vector<std::string> kDefault{"lighting", "game", "cpu", "gpu", "memory",
                                                   "fans",     "temps", "devices", "connections", "system"};
    return kDefault;
}

struct Prefs {
    Mode mode = Mode::Auto;
    Rgb manualColor{0, 140, 255};
    Rgb manualColor2{255, 255, 255};  // second color of gradient / comet / twinkle
    ManualEffect effect = ManualEffect::Static;
    float speedHz = 0.5f;  // effect cycles per second (0 = still, for gradients)
    // Rainbow look (color cycle, rainbow wave and the idle rainbow): see fx::Params.
    float rainbowHueStart = 0, rainbowHueSpan = 360, rainbowSaturation = 1;
    int rainbowSpread = 1;
    bool effectReverse = false;  // moving effects turn the other way
    IdleBehavior idle = IdleBehavior::ManualColor;
    // "Stop controlling the lights": remembered across restarts until resumed.
    bool lightingStopped = false;
    bool startMinimized = true;
    std::vector<Rgb> recentColors;  // most recent first, max kMaxRecent
    // Exe names (lower case) of games that have sent lighting before: shown as supporting
    // dynamic lighting even before they start sending.
    std::vector<std::string> lightingGames;
    // Games the user added on the Games List page (full exe paths).
    std::vector<std::wstring> manualGames;
    // Games without dynamic lighting show the screen's colors (else the idle choice).
    bool screenForUnsupported = false;
    // Per game (key: games::Normalize(name)): what it shows when it has no lighting of its own.
    std::map<std::string, GameMode> gameModes;
    std::map<std::string, Rgb> gameColors;  // for GameMode::Color
    std::vector<std::string> dashboard = DefaultDashboard();
    int lhmPort = 8085;  // LibreHardwareMonitor's web server
    // Light Logitech devices (G HUB) along with Aura.
    bool logitechDevices = true;
    // Light the ROG Azoth over its cable (experimental, opt-in).
    bool azothKeyboard = false;
    // Light HyperX / Kingston FURY RGB memory through the elevated RAM helper (experimental, opt-in).
    bool ramLighting = false;
    // What the RAM shows when LumaBridge lets go of it: 0 its own rainbow, 1 off, 2 the last color.
    int ramRelease = 0;
    // Per device (device::kFans, ...): its own look, or following the main one.
    std::map<std::string, DeviceLighting> deviceLighting;
    // The Lighting page's "Your setup" canvas: where each item sits (see DefaultSpot).
    std::map<std::string, Spot> setupSpots;
    static constexpr size_t kMaxRecent = 8;
};

// The main look (all devices that don't have their own), as stored in Prefs.
inline Look MainLook(const Prefs& p) {
    Look l;
    l.effect = p.effect;
    l.color1 = p.manualColor;
    l.color2 = p.manualColor2;
    l.speedHz = p.speedHz;
    l.hueStart = p.rainbowHueStart;
    l.hueSpan = p.rainbowHueSpan;
    l.saturation = p.rainbowSaturation;
    l.spread = p.rainbowSpread;
    l.reverse = p.effectReverse;
    return l;
}
inline void SetMainLook(Prefs& p, const Look& l) {
    p.effect = l.effect;
    p.manualColor = l.color1;
    p.manualColor2 = l.color2;
    p.speedHz = l.speedHz;
    p.rainbowHueStart = l.hueStart;
    p.rainbowHueSpan = l.hueSpan;
    p.rainbowSaturation = l.saturation;
    p.rainbowSpread = l.spread;
    p.effectReverse = l.reverse;
}
// A device's look right now: its own, or the main one.
inline Look DeviceLook(const Prefs& p, const std::string& id) {
    auto it = p.deviceLighting.find(id);
    return it != p.deviceLighting.end() && it->second.own ? it->second.look : MainLook(p);
}
inline Spot SetupSpot(const Prefs& p, const std::string& item) {
    auto it = p.setupSpots.find(item);
    return it != p.setupSpots.end() ? it->second : DefaultSpot(item);
}

Prefs LoadPrefs(const std::wstring& iniPath);
// Writes [App] plus the shared keys the app lets you edit.
void SaveAll(const std::wstring& iniPath, const Prefs& prefs, const Config& cfg);

std::string ToHex(Rgb c);                    // "#RRGGBB"
bool FromHex(const std::string& s, Rgb* out);  // accepts "#RRGGBB" or "RRGGBB"

bool IsAutostartEnabled();
bool SetAutostart(bool enable);  // HKCU Run entry: "<exe>" --minimized

}  // namespace luma::app
