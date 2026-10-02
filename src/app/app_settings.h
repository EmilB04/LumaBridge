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
#include "pad_mapping.h"
#include "azoth_oled.h"

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

// Dashboard cards below the fixed Lighting / Game row, in display order (ids; see ui.cpp).
// Cards not listed are hidden.
inline const std::vector<std::string>& DefaultDashboard() {
    static const std::vector<std::string> kDefault{"cpu",     "gpu",         "memory",  "power", "fans",
                                                   "temps",   "devices",     "connections", "storage", "system"};
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
    // Closing the window: hide it in the tray (keeps the lighting running), else exit.
    bool closeToTray = false;
    bool lighting3d = true;  // the Lighting page's preview: 3D, else the flat 2D one
    bool dashGraphs = true;  // the dashboard's performance cards show graphs
    int psuWatts = 0;        // the power supply's rating (W), as set by you (0: not set)
    // The setup guide has been finished (or skipped); until then the window opens on it.
    bool setupDone = false;
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
    int forzaPort = 5300;  // where Forza's Data Out sends (set the same in the game)
    int beamngPort = 4444;   // where BeamNG.drive's OutGauge sends
    int dirtPort = 20777;    // DiRT Rally / DiRT Rally 2.0's telemetry
    int ams2Port = 5606;     // Automobilista 2 / Project CARS 2's UDP
    int xplanePort = 49003;  // X-Plane's data output
    int f1Port = 20777;    // where the F1 games' telemetry sends (set the same in the game)
    int wrcPort = 49718;   // where EA SPORTS WRC sends LumaBridge's packets (written into its config.json)
    // Light Logitech devices (G HUB) along with Aura.
    bool logitechDevices = true;
    // Keep Logitech devices with LumaBridge even while a game lights them itself (through
    // LIGHTSYNC or G HUB); off: LumaBridge hands them to the game (e.g. Battlefield's own
    // lighting on the mouse).
    bool logitechForce = false;
    // Devices going to sleep (device_sleep.h): after this long without being used, the device's
    // lighting fades out and LumaBridge stops talking to it until it's used again. The mouse
    // also follows G HUB's "turn off lighting on inactivity" when G HUB answers.
    bool logitechSleep = true;
    int logitechSleepSec = 60;
    bool logitechSleepIgnoreDynamic = false;  // stay awake while an active game sends lighting
    bool azothSleep = true;
    int azothSleepSec = 300;
    // Default on: a game played with a controller never touches the keyboard, so the default
    // sleep timeout would fade it out mid-match despite active game lighting.
    bool azothSleepIgnoreDynamic = true;
    // Light the ROG Azoth (by cable or its Omni receiver).
    bool azothKeyboard = true;
    azoth::OledSettings azothOled;
    std::wstring azothOledBanner = L"LumaBridge";
    int azothOledBannerSize = 24;
    bool azothOledBannerInvert = false;
    // Light a Sony DualSense's lightbar, by USB or Bluetooth (experimental).
    bool padInput = true;  // read a connected PlayStation controller (live view, battery, report timing)
    pad::Mapping padMapping;  // its button mapping (keyboard / mouse), off until switched on
    // The controller page's fast-changing numbers (raw readings, report timing): hidden until switched on.
    bool padShowValues = false, padShowTiming = false;
    bool padView3d = false;  // the live view: 2D with every input (default), or the 3D model
    bool dualsenseController = true;
    // Light HyperX / Kingston FURY RGB memory through the elevated RAM helper.
    bool ramLighting = true;
    // Keep Armoury Crate's lighting service paused while LumaBridge has the lights (through the
    // hardware helper): it re-applies its own lighting when USB devices change.
    bool pauseArmouryCrate = true;
    // Experimental documented Kraken X3 pump / Z3 accessory lighting. LCDs stay with CAM.
    bool nzxtLighting = false;
    // Light devices with Windows' lighting standard built in (HID LampArray), directly.
    bool lampArray = true;
    // Per LampArray device (by HID identity): switched on or off by the user; the rest follow
    // Controller::LampArrayDefaultOn.
    std::map<std::string, bool> lampArrayDevices;
    // Light the devices OpenRGB supports, through its SDK server (only if OpenRGB runs).
    bool openRgb = false;
    int openRgbPort = 6742;
    // Per OpenRGB device (by serial / location identity): switched on or off by the user; the rest follow
    // Controller::OpenRgbDefaultOn.
    std::map<std::string, bool> openRgbDevices;
    // Which memory slots hold a stick (bit 0 A1 .. bit 3 B2, from the CPU outward), set by hand
    // on the Memory page; -1: as the system scan says.
    int ramSlots = -1;
    // What the RAM shows when LumaBridge lets go of it: 0 its own rainbow, 1 off, 2 the last color.
    int ramRelease = 0;
    // Per device (device::kFans, ...): its own look, or following the main one.
    std::map<std::string, DeviceLighting> deviceLighting;
    // Where things sit on the desk in the 3D views ("desk:case", "desk:keyboard", ...; see
    // pc::DeskItems). Also the old 2D canvas's spots (0.15 and earlier), kept as they were.
    std::map<std::string, Spot> setupSpots;
    // The case's fans and how it's turned (pc::Encode); "" until you correct LumaBridge's guess.
    std::string caseLayout;
    std::map<std::string, float> monitorSizes;  // monitor identity (displays::SizeKey) -> diagonal in inches
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
// A device's own brightness (0..1), on top of the overall one.
inline float DeviceBrightness(const Prefs& p, const std::string& id) {
    auto it = p.deviceLighting.find(id);
    return it != p.deviceLighting.end() ? it->second.brightness : 1.f;
}
// The device's effects run the other way (DeviceLighting::reverse).
inline bool DeviceReversed(const Prefs& p, const std::string& id) {
    auto it = p.deviceLighting.find(id);
    return it != p.deviceLighting.end() && it->second.reverse;
}
// Handed back to its own app (DeviceLighting::native).
inline bool DeviceNative(const Prefs& p, const std::string& id) {
    auto it = p.deviceLighting.find(id);
    return it != p.deviceLighting.end() && it->second.native;
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
