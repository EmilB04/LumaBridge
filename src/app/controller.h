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

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app_settings.h"
#include "aura_mirror.h"
#include "config.h"
#include "gamesense_server.h"
#include "ipc.h"
#include "source_tracker.h"

namespace luma::app {

class Controller {
public:
    // What is currently being shown, for the UI.
    struct Output {
        enum class Kind { ArmouryCrate, Static, Breathing, Strobe } kind = Kind::ArmouryCrate;
        Rgb color{};
        double hz = 0;
        std::string label;  // "Battlefield 1 - Logitech LIGHTSYNC", "Manual color", ...

        bool SameLighting(const Output& o) const {
            return kind == o.kind && color == o.color && hz == o.hz;
        }
    };

    bool Init();
    void Shutdown();

    void OnIpc(const ipc::Frame& f);
    void Tick();  // call every ~50 ms

    // Settings: mutate, then call Changed() so they are applied and (debounced) saved.
    Prefs& prefs() { return prefs_; }
    Config& config() { return cfg_; }
    void Changed();
    void RememberManualColor();  // push the current manual color onto the recents list

    void SetGameSenseEnabled(bool enabled);

    // True when the previous run ended without a clean exit while LumaBridge controlled
    // Aura: lighting control stays off until the user resumes it, so a crash in the Aura
    // SDK can't turn into a crash loop.
    bool auraPaused() const { return auraPaused_; }
    void ResumeAura();
    void RescanDevices() { mirror_.Rescan(); }

    const Output& output() const { return output_; }
    const std::vector<Source>& sources() const { return tracker_.All(); }
    AuraMirror::Status auraStatus() const { return mirror_.GetStatus(); }
    const gamesense::Server& gameSense() const { return gameSense_; }
    const std::wstring& configPath() const { return iniPath_; }
    const std::wstring& logPath() const { return cfg_.logFile; }

private:
    Output Decide() const;
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
    Output output_;
    bool outputApplied_ = false;
    bool dirty_ = false;
    uint64_t dirtySince_ = 0;
    std::map<uint32_t, std::string> processNames_;
};

}  // namespace luma::app
