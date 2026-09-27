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

        bool SameLighting(const Output& o) const {
            return stopped == o.stopped && fanTest == o.fanTest && fx.kind == o.fx.kind &&
                   fx.color1 == o.fx.color1 && fx.color2 == o.fx.color2 && fx.speed == o.fx.speed;
        }
    };

    bool Init();
    // Idempotent. `handBack`: start Armoury Crate so it takes the lights back.
    void Shutdown(bool handBack = true);

    void OnIpc(const ipc::Frame& f);
    void Tick();  // call every ~50 ms

    // Settings: mutate, then call Changed() so they are applied and (debounced) saved.
    Prefs& prefs() { return prefs_; }
    Config& config() { return cfg_; }
    void Changed();
    void RememberManualColor();  // push the current manual color onto the recents list

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
    // Install folder of a known game (by profile key, e.g. "cs2"), from the Games List scan.
    std::wstring GameDir(const char* profileKey) const;
    void RefreshFeedSettings();  // re-read Rocket League's Stats API port after setup changes
    // Screen colors is running for a game right now.
    bool screenColorsActive() const { return screen_.Running(); }

    // Logitech devices through G HUB.
    const LogitechOutput& logitech() const { return logitech_; }
    void SetLogitechEnabled(bool on);
    // Why LumaBridge isn't lighting Logitech devices right now ("" when it is).
    const std::string& logitechNote() const { return logitechNote_; }

    // ASUS ROG Azoth over USB (wired).
    const AzothOutput& azoth() const { return azoth_; }
    void SetAzothEnabled(bool on);

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
    uint64_t handbackDoneAt_ = 0;  // GetTickCount64() when the current hand-back should be done
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
    std::string logitechNote_;
    void UpdateLogitech();
    bool feedActive_[3] = {};
    std::future<std::vector<InstalledGame>> libraryJob_;
    bool libraryRescanPending_ = false;  // the list changed while a scan was running
};

}  // namespace luma::app
