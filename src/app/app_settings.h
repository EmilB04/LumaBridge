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
#include "effects.h"

namespace luma::app {

enum class Mode { Auto, Manual };
// Manual effects are the per-LED effects (ColorCycle is the classic "rainbow": all LEDs one
// hue; RainbowWave spreads the hues around each fan).
using ManualEffect = fx::Kind;
// What Auto mode shows when no game is sending lighting. ArmouryCrate hands the lights back
// (stop + start Armoury Crate so it re-applies its effect).
enum class IdleBehavior { ManualColor, Rainbow, Off, ArmouryCrate };

// Per-game choice on the Games List page.
enum class GameMode { Default, Screen, Idle };

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
    std::vector<std::string> dashboard = DefaultDashboard();
    int lhmPort = 8085;  // LibreHardwareMonitor's web server
    static constexpr size_t kMaxRecent = 8;
};

Prefs LoadPrefs(const std::wstring& iniPath);
// Writes [App] plus the shared keys the app lets you edit.
void SaveAll(const std::wstring& iniPath, const Prefs& prefs, const Config& cfg);

std::string ToHex(Rgb c);                    // "#RRGGBB"
bool FromHex(const std::string& s, Rgb* out);  // accepts "#RRGGBB" or "RRGGBB"

bool IsAutostartEnabled();
bool SetAutostart(bool enable);  // HKCU Run entry: "<exe>" --minimized

}  // namespace luma::app
