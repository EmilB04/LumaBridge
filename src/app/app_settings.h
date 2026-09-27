// App preferences ([App] section of %LOCALAPPDATA%\LumaBridge\LumaBridge.ini) and the
// "start with Windows" registry entry. Shared settings (calibration, devices, ...) live in
// luma::Config so the game DLLs read the same values when the app isn't running.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "color.h"
#include "config.h"

namespace luma::app {

enum class Mode { Auto, Manual };
enum class ManualEffect { Static, Breathing, Strobe };
enum class IdleBehavior { ArmouryCrate, ManualColor };

struct Prefs {
    Mode mode = Mode::Auto;
    Rgb manualColor{0, 140, 255};
    ManualEffect effect = ManualEffect::Static;
    float speedHz = 0.5f;  // breathing cycles / strobe flashes per second
    IdleBehavior idle = IdleBehavior::ArmouryCrate;
    bool startMinimized = true;
    std::vector<Rgb> recentColors;  // most recent first, max kMaxRecent
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
