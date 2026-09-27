#include "controller.h"

#include <algorithm>
#include <cctype>
#include <iterator>

#include "armoury_crate.h"
#include "log.h"
#include "usb_aura.h"

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
    RescanDevices();
    return true;
}

void Controller::RescanDevices() {
    if (mirror_.IsRunning()) {
        mirror_.Rescan();
        return;
    }
    if (cfg_.auraUseSdk) return;  // the SDK can't be listed without taking control
    knownDevices_ = aurausb::ProbeDevices();
    LUMA_INFO("device scan (read-only): %d device(s)", static_cast<int>(knownDevices_.size()));
}

const std::vector<AuraDeviceInfo>& Controller::devices() {
    auto st = mirror_.GetStatus();
    if (!st.devices.empty()) knownDevices_ = st.devices;
    return knownDevices_;
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
    auto lit = [](const char* label) {
        Output o;
        o.stopped = false;
        o.label = label;
        return o;
    };
    auto manual = [&](const char* label) {
        Output o = lit(label);
        o.fx.kind = prefs_.effect;
        o.fx.color1 = prefs_.manualColor;
        o.fx.color2 = prefs_.manualColor2;
        o.fx.speed = prefs_.effect == ManualEffect::Static ? 0 : prefs_.speedHz;
        return o;
    };

    if (auraPaused_) {
        Output o;
        o.label = "Paused after an unexpected exit";
        return o;
    }
    Output o = [&] {
        if (prefs_.lightingStopped) {
            Output s;
            s.label = "Stopped - LumaBridge isn't controlling the lights";
            return s;
        }
        if (prefs_.mode == Mode::Manual) return manual("Manual color");

        if (auto s = tracker_.Active()) {
            std::string name = s->game;
            for (const GameStatus& gs : games_)
                if (SourceBelongsTo(*s, gs.game)) name = gs.game.name;
            Output g = lit((name + " - " + s->sdk).c_str());
            g.fx.kind = s->flashHz > 0 ? fx::Kind::Strobe : fx::Kind::Static;
            g.fx.color1 = s->color;
            g.fx.speed = s->flashHz;
            return g;
        }
        // A game without (or not yet sending) dynamic lighting is running: say so.
        std::string idle = "No game running";
        if (!games_.empty()) {
            const GameStatus& gs = games_.front();
            idle = gs.game.name + (games::SupportsLighting(gs.support) ? " - waiting for its lighting"
                                                                       : " - no dynamic lighting");
        }
        auto withIdle = [&](const char* what) { return idle + " - " + what; };
        switch (prefs_.idle) {
        case IdleBehavior::Rainbow: {
            Output r = lit(withIdle("rainbow").c_str());
            r.fx.kind = fx::Kind::RainbowWave;
            r.fx.color1 = Rgb{170, 60, 255};  // icon tint; the effect itself is every hue
            r.fx.speed = 0.1;                 // one slow turn every 10 s
            return r;
        }
        case IdleBehavior::Off: {
            Output off = lit(withIdle("lights off").c_str());
            off.fx.color1 = Rgb{};
            return off;
        }
        case IdleBehavior::ArmouryCrate: {
            Output ac;  // stopped: hand back
            ac.label = withIdle("Armoury Crate");
            return ac;
        }
        default:
            return manual(withIdle("your color").c_str());
        }
    }();
    if (fanTest_) {
        // The test pattern shows even while the lights are otherwise handed back.
        if (o.stopped) o = manual("Fan layout test");
        o.fanTest = true;
        o.label = "Fan layout test";
    }
    return o;
}

bool Controller::SourceBelongsTo(const Source& s, const RunningGame& g) const {
    if (s.pid != 0) return s.pid == g.pid;
    // GameSense (in-process server, no pid): match its game name ("ROCKETLEAGUE").
    const std::string n = games::Normalize(s.game);
    if (n.empty()) return false;
    std::string stem = g.exe.substr(0, g.exe.find_last_of('.'));
    return n == games::Normalize(g.name) || n == games::Normalize(stem) || n == games::Normalize(g.folder);
}

std::vector<Source> Controller::unmatchedSources() const {
    std::vector<Source> out;
    for (const Source& s : tracker_.All()) {
        bool matched = false;
        for (const GameStatus& g : games_) matched |= SourceBelongsTo(s, g.game);
        if (!matched) out.push_back(s);
    }
    return out;
}

void Controller::UpdateGames(uint64_t now) {
    detector_.Poll(now);
    std::vector<GameStatus> next;
    for (const RunningGame& g : detector_.Games()) {
        GameStatus st;
        st.game = g;
        st.sdk = g.sdk;
        const Source* sending = nullptr;
        for (const Source& s : tracker_.All())
            if (SourceBelongsTo(s, g) && (!sending || s.lastChange > sending->lastChange)) sending = &s;
        if (sending) {
            st.sdk = sending->sdk;
            st.color = sending->color;
        }
        std::string exe = g.exe;
        for (auto& c : exe) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto& known = prefs_.lightingGames;
        const bool seen = std::find(known.begin(), known.end(), exe) != known.end();
        if (sending && !seen && !exe.empty()) {
            known.push_back(exe);
            LUMA_INFO("games: %s supports dynamic lighting (%s) - remembered", g.name.c_str(), sending->sdk.c_str());
            dirty_ = true;
            dirtySince_ = now;
        }
        st.support = games::ClassifySupport(sending != nullptr, seen, !g.sdk.empty(), g.modulesReadable);
        next.push_back(std::move(st));
    }
    games_ = std::move(next);
}

void Controller::Apply(const Output& out) {
    if (out.stopped) {
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
    mirror_.SetPattern(out.fx, cfg_.argbFans, out.fanTest);
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
    UpdateGames(now);

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
