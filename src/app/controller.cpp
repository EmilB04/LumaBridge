#include "controller.h"

#include <algorithm>
#include <iterator>

#include "armoury_crate.h"
#include "log.h"

namespace luma::app {
namespace {

constexpr uint64_t kSaveDebounceMs = 600;

}  // namespace

bool Controller::Init() {
    iniPath_ = UserConfigPath();
    // The app reads only the per-user config (a LumaBridge.ini next to a game DLL is a
    // per-game override for that DLL's fallback mode, not for the app).
    cfg_ = LoadConfig(L"");
    prefs_ = LoadPrefs(iniPath_);
    if (!cfg_.logFile.empty()) {
        size_t dot = cfg_.logFile.find_last_of(L'.');
        cfg_.logFile = cfg_.logFile.substr(0, dot) + L"-app.log";
    }
    log::Init(cfg_.logFile, cfg_.logLevel);
    LUMA_INFO("==== LumaBridge app started (pid %lu) ====", GetCurrentProcessId());

    // Crash-loop guard: [App] Running is 1 while the app runs and 0 after a clean exit.
    wchar_t running[8] = {};
    GetPrivateProfileStringW(L"App", L"Running", L"0", running, 8, iniPath_.c_str());
    if (running[0] == L'1') {
        auraPaused_ = true;
        LUMA_WARN("previous run did not exit cleanly - Aura control paused until resumed");
    }
    WriteConfigValue(iniPath_, L"App", L"Running", L"1");

    if (cfg_.gameSenseEnabled)
        gameSense_.Start(cfg_, [] { /* picked up by the next Tick */ });
    return true;
}

void Controller::ResumeAura() {
    LUMA_INFO("lighting control resumed by the user");
    auraPaused_ = false;
    if (prefs_.lightingStopped) {
        prefs_.lightingStopped = false;
        Changed();
    }
    outputApplied_ = false;
}

void Controller::StopLighting() {
    LUMA_INFO("lighting control stopped by the user");
    prefs_.lightingStopped = true;
    Changed();
}

void Controller::Shutdown(bool handBack) {
    if (shutDown_) return;
    shutDown_ = true;
    if (dirty_) SaveAll(iniPath_, prefs_, cfg_);
    gameSense_.Stop();
    const bool wasControlling = mirror_.IsRunning();
    mirror_.Stop();
    // Exiting LumaBridge gives the lights back to Armoury Crate (not during a Windows
    // shutdown: the controller reloads Armoury Crate's saved effect at the next boot anyway).
    if (wasControlling && handBack) HandBackLighting();
    WriteConfigValue(iniPath_, L"App", L"Running", L"0");
    LUMA_INFO("LumaBridge app exiting");
}

void Controller::Changed() {
    if (mirror_.IsRunning() && cfg_.maxUpdateHz != mirrorHz_) mirror_.Stop();  // restarted by Tick
    mirror_.SetCorrection(cfg_.auraCorrection);
    mirror_.SetDisabledDevices(cfg_.auraDisabledDevices);
    outputApplied_ = false;  // re-apply manual color / effect edits right away
    dirty_ = true;
    dirtySince_ = GetTickCount64();
}

void Controller::RememberManualColor() {
    auto& r = prefs_.recentColors;
    r.erase(std::remove(r.begin(), r.end(), prefs_.manualColor), r.end());
    r.insert(r.begin(), prefs_.manualColor);
    if (r.size() > Prefs::kMaxRecent) r.resize(Prefs::kMaxRecent);
    Changed();
}

void Controller::SetGameSenseEnabled(bool enabled) {
    cfg_.gameSenseEnabled = enabled;
    if (enabled) gameSense_.Start(cfg_, [] {});
    else gameSense_.Stop();
    Changed();
}

std::string Controller::ProcessName(uint32_t pid) {
    auto it = processNames_.find(pid);
    if (it != processNames_.end()) return it->second;
    std::string name = "PID " + std::to_string(pid);
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2];
        DWORD size = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(h, 0, path, &size)) {
            std::wstring p(path, size);
            p = p.substr(p.find_last_of(L"\\/") + 1);
            if (p.size() > 4 && _wcsicmp(p.c_str() + p.size() - 4, L".exe") == 0) p.resize(p.size() - 4);
            name.clear();
            for (wchar_t c : p) name += static_cast<char>(c < 128 ? c : '?');
        }
        CloseHandle(h);
    }
    processNames_[pid] = name;
    return name;
}

void Controller::OnIpc(const ipc::Frame& f) {
    const uint64_t now = GetTickCount64();
    if (f.kind == ipc::FrameKind::Release) {
        LUMA_INFO("source released: %s (pid %u)", f.source, f.pid);
        tracker_.OnRelease(f.pid, f.source);
        processNames_.erase(f.pid);
    } else {
        tracker_.OnFrame(f.pid, f.source, ProcessName(f.pid), Rgb{f.r, f.g, f.b}, 0, now);
    }
}

Controller::Output Controller::Decide() const {
    auto manual = [this](const char* label) {
        Output o;
        o.color = prefs_.manualColor;
        o.hz = prefs_.speedHz;
        o.label = label;
        switch (prefs_.effect) {
        case ManualEffect::Breathing: o.kind = Output::Kind::Breathing; break;
        case ManualEffect::Strobe: o.kind = Output::Kind::Strobe; break;
        case ManualEffect::Rainbow: o.kind = Output::Kind::Rainbow; break;
        default: o.kind = Output::Kind::Static; o.hz = 0; break;
        }
        return o;
    };

    if (auraPaused_) {
        Output o;
        o.label = "Paused after an unexpected exit";
        return o;
    }
    if (prefs_.lightingStopped) {
        Output o;
        o.label = "Stopped - LumaBridge isn't controlling the lights";
        return o;
    }
    if (prefs_.mode == Mode::Manual) return manual("Manual color");

    if (auto s = tracker_.Active()) {
        Output o;
        o.kind = s->flashHz > 0 ? Output::Kind::Strobe : Output::Kind::Static;
        o.color = s->color;
        o.hz = s->flashHz;
        o.label = s->game + " - " + s->sdk;
        return o;
    }
    switch (prefs_.idle) {
    case IdleBehavior::Rainbow: {
        Output o;
        o.kind = Output::Kind::Rainbow;
        o.color = Rgb{170, 60, 255};  // icon/preview tint; the effect itself cycles
        o.hz = 0.1;                   // one slow cycle every 10 s
        o.label = "No game running - rainbow";
        return o;
    }
    case IdleBehavior::Off: {
        Output o;
        o.kind = Output::Kind::Static;
        o.label = "No game running - lights off";
        return o;
    }
    case IdleBehavior::ArmouryCrate: {
        Output o;  // Kind::Stopped: hand back
        o.label = "No game running - Armoury Crate";
        return o;
    }
    default:
        return manual("No game running - your color");
    }
}

void Controller::Apply(const Output& out) {
    if (out.kind == Output::Kind::Stopped) {
        if (mirror_.IsRunning()) {
            LUMA_INFO("no longer controlling the lights - handing back to Armoury Crate");
            mirror_.Stop();
            // After a crash-loop pause LumaBridge never took the lights, so nothing to hand back.
            if (!auraPaused_) HandBackLighting();
        }
        return;
    }
    if (!mirror_.IsRunning()) {
        Config c = cfg_;
        c.releaseControlOnShutdown = true;
        mirror_.Start(c, nullptr, "LumaBridge app", /*routeToApp=*/false);
        mirrorHz_ = c.maxUpdateHz;
    }
    switch (out.kind) {
    case Output::Kind::Breathing:
        mirror_.Pulse(out.color, 0, static_cast<int>(1000.0 / out.hz));
        break;
    case Output::Kind::Strobe:
        mirror_.Flash(out.color, 0, static_cast<int>(500.0 / out.hz));
        break;
    case Output::Kind::Rainbow:
        mirror_.Spectrum(static_cast<int>(1000.0 / out.hz));
        break;
    default:
        mirror_.SetStatic(out.color);
        break;
    }
}

void Controller::Tick() {
    const uint64_t now = GetTickCount64();

    if (gameSense_.IsRunning()) {
        auto snap = gameSense_.Poll(now);
        if (snap.version != gameSenseVersion_) {
            gameSenseVersion_ = snap.version;
            if (snap.ambient)
                tracker_.OnFrame(0, "SteelSeries GameSense", snap.ambient->game, snap.ambient->color,
                                 snap.ambient->flashHz, now);
            else
                tracker_.OnRelease(0, "SteelSeries GameSense");
        } else if (snap.ambient) {
            tracker_.OnFrame(0, "SteelSeries GameSense", snap.ambient->game, snap.ambient->color,
                             snap.ambient->flashHz, now);  // keep-alive while the game is active
        }
    }
    tracker_.Prune(now, [](const Source&) { return false; });

    Output next = Decide();
    if (!outputApplied_ || !next.SameLighting(output_)) {
        Apply(next);
        outputApplied_ = true;
    }
    output_ = next;

    if (dirty_ && now - dirtySince_ >= kSaveDebounceMs) {
        SaveAll(iniPath_, prefs_, cfg_);
        dirty_ = false;
    }
}

}  // namespace luma::app
