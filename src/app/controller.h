// The app's brain: owns Aura (through an AuraMirror that never routes to itself), collects
// sources (game DLLs over IPC + the built-in GameSense server) and decides, every tick,
// what the lights show:
//
//   Manual mode   -> your color / effect
//   Auto mode     -> the most recently active game; when none is running either your color
//                    or Armoury Crate's own effects (Prefs::idle)
//
// UI-thread only, except the GameSense server which has its own threads.
#pragma once

#include <windows.h>

#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app_settings.h"
#include "aura_mirror.h"
#include "config.h"
#include "game_catalog.h"
#include "game_detector.h"
#include "game_feeds.h"
#include "game_library.h"
#include "game_profiles.h"
#include "screen_capture.h"
#include "system_monitor.h"
#include "logitech_output.h"
#include "azoth_output.h"
#include "dualsense_output.h"
#include "pad_input.h"
#include "openrgb_output.h"
#include "lamparray_output.h"
#include "hardware_helper.h"
#include "nzxt_kraken.h"
#include "device_catalog.h"
#include "device_inventory.h"
#include "gamesense_server.h"
#include "ipc.h"
#include "source_tracker.h"

namespace luma::app {

class Controller {
public:
    // What is currently being shown, for the UI.
    struct Output {
        // Stopped: LumaBridge isn't driving the lights (paused or stopped by the user).
        bool stopped = true;
        fx::Params fx;         // the effect (games: Static, or Strobe while they flash)
        bool fanTest = false;  // fans show the layout test pattern (Devices page)
        std::string label;     // "Battlefield 1 - Logitech LIGHTSYNC", "Manual color", ...
        bool game = false;     // a game's own lighting: every device in step, the mouse LED by LED
        // Devices with their own look (device::kFans, ...) while your own lighting shows;
        // the others show `fx`.
        std::map<std::string, fx::Params> devices;

        const fx::Params& For(const std::string& id) const {
            auto it = devices.find(id);
            return it != devices.end() ? it->second : fx;
        }
        bool SameLighting(const Output& o) const {
            if (stopped != o.stopped || fanTest != o.fanTest || !fx::SameParams(fx, o.fx) ||
                devices.size() != o.devices.size())
                return false;
            for (const auto& [id, p] : devices) {
                auto it = o.devices.find(id);
                if (it == o.devices.end() || !fx::SameParams(p, it->second)) return false;
            }
            return true;
        }
    };

    bool Init();
    // Idempotent. `handBack`: start Armoury Crate so it takes the lights back.
    void Shutdown(bool handBack = true);

    void OnIpc(const ipc::Frame& f);
    // Input from a device (raw input, main.cpp): when it was last used, for letting it sleep.
    void OnDeviceInput(uint16_t vid, uint16_t pid);
    bool logitechAsleep() const { return logitechAsleep_; }
    bool azothAsleep() const { return azothAsleep_; }
    // G HUB's "turn off lighting on inactivity" (nullopt: G HUB didn't answer).
    std::optional<bool> ghubSleep() const { return presence_.ghubSleep; }
    void Tick();  // call every ~50 ms

    // Settings: mutate, then call Changed() so they are applied and (debounced) saved.
    Prefs& prefs() { return prefs_; }
    const Prefs& prefs() const { return prefs_; }
    Config& config() { return cfg_; }
    void Changed();
    void RememberManualColor();  // push the current manual color onto the recents list
    void RememberColor(Rgb c);

    void SetGameSenseEnabled(bool enabled);

    // Fan layout test pattern (Devices page); not saved.
    void SetFanTest(bool on) { fanTest_ = on; }
    bool fanTest() const { return fanTest_; }

    // Lighting control is off either because the previous run didn't exit cleanly (crash-loop
    // guard) or because the user chose "Stop controlling the lights". ResumeAura() clears both.
    enum class Pause { None, AfterCrash, ByUser };
    Pause pause() const {
        return auraPaused_ ? Pause::AfterCrash : prefs_.lightingStopped ? Pause::ByUser : Pause::None;
    }
    bool auraPaused() const { return pause() != Pause::None; }
    void ResumeAura();
    void StopLighting();
    // While Armoury Crate is taking the lights back (the controller restarts, ~10 s): the
    // time left, else 0. Also the total, for a progress bar.
    uint64_t handbackMsLeft() const;
    static constexpr uint64_t kHandbackMs = 10000;
    // Re-lists devices: through the running mirror, or with a read-only probe when
    // LumaBridge isn't controlling the lights (so Armoury Crate keeps them).
    void RescanDevices();
    // Devices seen most recently (live while controlling, else the last probe).
    const std::vector<AuraDeviceInfo>& devices();

    // A running game and whether it does dynamic lighting.
    struct GameStatus {
        RunningGame game;
        games::Support support = games::Support::None;
        std::string sdk;   // the SDK it uses, when known
        Rgb color{};       // its current color while Active
        const games::GameProfile* profile = nullptr;  // what LumaBridge knows about it
        GameMode mode = GameMode::Default;             // the user's choice for it
    };
    const std::vector<GameStatus>& games() const { return games_; }
    // Sources that don't belong to any detected game (e.g. a game outside the known libraries).
    std::vector<Source> unmatchedSources() const;

    // Games installed on this PC (Games List page), scanned in the background.
    const std::vector<InstalledGame>& library() const { return library_; }
    bool libraryScanning() const { return libraryJob_.valid(); }
    void RescanLibrary();
    void AddManualGame(const std::wstring& exePath);
    void RemoveManualGame(const std::wstring& exePath);

    // Built-in feeds and their setup (Integrations page).
    const GameFeeds& feeds() const { return feeds_; }
    // What a device shows: its effect in `out`, turned round if the device is set to run the
    // other way.
    fx::Params DeviceEffect(const Output& out, const std::string& id) const;
    fx::Params DeviceEffect(const std::string& id) const { return DeviceEffect(output_, id); }
    // Install folder of a known game (by profile key, e.g. "cs2"), from the Games List scan.
    std::wstring GameDir(const char* profileKey) const;
    void RefreshFeedSettings();  // re-read Rocket League's Stats API port after setup changes
    // Screen colors is running for a game right now.
    bool screenColorsActive() const { return screen_.Running(); }
    // Why Screen colors can't read the screen right now ("" when it can).
    std::string screenProblem() const { return screen_.Running() ? screen_.problem() : std::string(); }

    // Logitech devices through G HUB.
    const LogitechOutput& logitech() const { return logitech_; }
    void SetLogitechEnabled(bool on);
    // Why LumaBridge isn't lighting Logitech devices right now ("" when it is).
    const std::string& logitechNote() const { return logitechNote_; }

    // What's plugged in, found in the background (at start, on RescanDevices and every
    // minute): the pages show only devices this PC has.
    struct Presence {
        bool scanned = false;  // the first scan has finished
        bool azoth = false;    // ROG Azoth, by cable or its Omni receiver
        bool dualsense = false;  // a DualSense controller, by USB or Bluetooth
        inventory::Scan inventory;
        // Every USB device, as (vendor, product): for naming RGB brands (device_catalog.h).
        std::vector<std::pair<uint16_t, uint16_t>> usb;
        // Krakens found by their USB name (any product ID), and the AIO cooler found.
        std::vector<std::pair<uint16_t, uint16_t>> krakens;
        const catalog::AioModel* Aio() const { return catalog::FindAio(usb, krakens); }
        std::optional<bool> ghubSleep;  // G HUB's "turn off lighting on inactivity"
        std::vector<LogitechDevice> logitech;  // with and without RGB lighting
        // Logitech devices with RGB lighting.
        std::vector<LogitechDevice> LogitechRgb() const {
            std::vector<LogitechDevice> out;
            for (const auto& d : logitech)
                if (d.rgb) out.push_back(d);
            return out;
        }
    };
    const Presence& presence() const { return presence_; }
    bool presenceScanning() const { return presenceJob_.valid(); }
    // An NZXT Kraken's own readings (liquid temperature, pump and fan speeds), when one is plugged in.
    nzxt::Status kraken() const { return kraken_.status(); }
    nzxt::KrakenState krakenState() const { return kraken_.state(); }
    bool krakenListening() const { return kraken_.listening(); }
    unsigned long krakenError() const { return kraken_.lastError(); }
    bool krakenLightingActive() const { return kraken_.lightingActive(); }
    unsigned long krakenLightingError() const { return kraken_.lightingError(); }
    void RescanPresence();

    // ASUS ROG Azoth over USB (wired).
    const AzothOutput& azoth() const { return azoth_; }
    void SetAzothEnabled(bool on);
    void SetAzothOled(azoth::OledSettings settings);
    void ReapplyAzothOled() { azoth_.ReapplyOled(); }
    // A USB device came or went, or the PC woke up (from the window's WM_DEVICECHANGE /
    // WM_POWERBROADCAST): look for the Azoth again, and take the motherboard's lights back if
    // Armoury Crate re-applies its lighting.
    void OnHardwareChanged() {
        azoth_.Rescan();
        // Not needed while Armoury Crate's lighting service is paused (it can't interfere then).
        if (mirror_.IsRunning() && hardware_.asusLighting() != helper::AsusLighting::Paused) mirror_.Reclaim();
    }

    // Sony DualSense lightbar, by USB or Bluetooth (experimental).
    const DualSenseOutput& dualsense() const { return dualsense_; }
    const PadInput& pad() const { return pad_; }
    void SetPadInputEnabled(bool on);
    void SetDualSenseEnabled(bool on);

    // Devices with Windows' lighting standard built in (HID LampArray), lit directly.
    const LampArrayOutput& lampArray() const { return lampArray_; }
    void SetLampArrayEnabled(bool on);
    // Whether LumaBridge lights this LampArray device: the user's choice, else on unless
    // LumaBridge already lights it another way (Aura, Logitech, the Azoth).
    bool LampArrayOn(const LampArrayDevice& d) const;
    bool LampArrayDefaultOn(const LampArrayDevice& d) const;

    // Devices OpenRGB supports, through its SDK server (optional).
    const OpenRgbOutput& openRgb() const { return openRgb_; }
    void SetOpenRgbEnabled(bool on);
    void SetOpenRgbPort(int port);
    // Whether LumaBridge lights this OpenRGB device: the user's choice, else on unless
    // LumaBridge already lights it itself (Aura, Logitech, the Azoth, FURY / HyperX memory).
    bool OpenRgbOn(const OpenRgbDevice& d) const;
    bool OpenRgbDefaultOn(const OpenRgbDevice& d) const;
    // HyperX / Kingston FURY RGB memory, through the elevated RAM helper.
    // The hardware helper: RAM lighting and the built-in sensors.
    HardwareHelper& hardware() { return hardware_; }
    const HardwareHelper& hardware() const { return hardware_; }
    void SetRamEnabled(bool on);

    // Dashboard data (CPU, GPU, memory, sensors, component names).
    sensors::SystemMonitor& monitor() { return monitor_; }

    const Output& output() const { return output_; }
    const std::vector<Source>& sources() const { return tracker_.All(); }
    AuraMirror::Status auraStatus() const { return mirror_.GetStatus(); }
    const gamesense::Server& gameSense() const { return gameSense_; }
    const std::wstring& configPath() const { return iniPath_; }
    const std::wstring& logPath() const { return cfg_.logFile; }

private:
    Output Decide() const;
    void RainbowLook(fx::Params* p) const;  // the rainbow settings from Prefs
    void UpdateGames(uint64_t now);
    void UpdateFeeds(uint64_t now);
    // The running game whose lighting falls back to the screen's colors, if any.
    const GameStatus* ScreenColorsGame() const;
    bool SourceBelongsTo(const Source& s, const RunningGame& g) const;
    void Apply(const Output& out);
    std::string ProcessName(uint32_t pid);

    Config cfg_;
    Prefs prefs_;
    std::wstring iniPath_;

    AuraMirror mirror_;
    gamesense::Server gameSense_;
    SourceTracker tracker_;
    uint64_t gameSenseVersion_ = ~0ull;

    int mirrorHz_ = 0;
    bool auraPaused_ = false;
    bool fanTest_ = false;
    bool AuraNative() const;
    bool auraNativeApplied_ = false;  // Apply() handed the Aura devices back to Armoury Crate
    uint64_t handbackDoneAt_ = 0;  // GetTickCount64() when the current hand-back should be done
    uint64_t armouryCheckAt_ = 0;  // when to look for Armoury Crate's window again
    bool armouryOpen_ = false;     // its window is open: its lighting service runs
    bool shutDown_ = false;
    Output output_;
    bool outputApplied_ = false;
    bool dirty_ = false;
    uint64_t dirtySince_ = 0;
    std::map<uint32_t, std::string> processNames_;
    std::vector<AuraDeviceInfo> knownDevices_;
    GameDetector detector_;
    std::vector<GameStatus> games_;
    std::vector<InstalledGame> library_;
    GameFeeds feeds_;
    ScreenCapture screen_;
    sensors::SystemMonitor monitor_;
    LogitechOutput logitech_;
    AzothOutput azoth_;
    DualSenseOutput dualsense_;
    PadInput pad_;
    OpenRgbOutput openRgb_;
    LampArrayOutput lampArray_;
    HardwareHelper hardware_;
    uint64_t sensorsPushedAt_ = 0;
    std::string logitechNote_;
    void UpdateLogitech();
    bool feedActive_[18] = {};
    std::future<std::vector<InstalledGame>> libraryJob_;
    Presence presence_;
    std::string presenceLogged_;  // what the log last said about NZXT devices
    nzxt::Kraken kraken_;
    uint64_t logitechInputAt_ = 0, azothInputAt_ = 0;  // last used (GetTickCount64)
    bool memoryLogged_ = false;  // the memory slots' names are in the log
    bool logitechAsleep_ = false, azothAsleep_ = false;
    // The mouse's and the keyboard's sleep timeouts right now (0: they don't sleep).
    // `dynamicActive`: an effect other than a plain static color is currently showing on it.
    uint64_t LogitechSleepMs(bool dynamicActive) const;
    uint64_t AzothSleepMs(bool dynamicActive) const;
    std::future<Presence> presenceJob_;
    uint64_t presenceAt_ = 0;  // when the last presence scan started
    bool libraryRescanPending_ = false;  // the list changed while a scan was running
};

}  // namespace luma::app
