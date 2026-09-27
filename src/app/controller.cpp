#include "controller.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>

#include "armoury_crate.h"
#include "integrations.h"
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
    detector_.SetExtraGames(prefs_.manualGames);
    RescanLibrary();
    feeds_.Start();
    monitor_.Start(prefs_.lhmPort);
    if (prefs_.logitechDevices) logitech_.Start(AppDirectory() + L"\\integrations\\LumaBridge_x64.dll");
    if (prefs_.azothKeyboard) azoth_.Start();
    if (prefs_.ramLighting) ram_.Start();
    return true;
}

void Controller::SetRamEnabled(bool on) {
    prefs_.ramLighting = on;
    if (on) ram_.Start();
    else ram_.Stop();
    Changed();
}

void Controller::SetAzothEnabled(bool on) {
    prefs_.azothKeyboard = on;
    if (on) azoth_.Start();
    else azoth_.Stop();
    Changed();
}

void Controller::SetLogitechEnabled(bool on) {
    prefs_.logitechDevices = on;
    if (on) logitech_.Start(AppDirectory() + L"\\integrations\\LumaBridge_x64.dll");
    else logitech_.Stop();  // G HUB takes its profile back
    Changed();
}

void Controller::UpdateLogitech() {
    if (!prefs_.logitechDevices) {
        logitechNote_ = "Turned off";
        return;
    }
    bool own = !output_.stopped;
    logitechNote_ = own ? "" : "LumaBridge isn't controlling the lights - G HUB has them";
    if (own)
        if (auto s = tracker_.Active(); s && s->sdk == "Logitech LIGHTSYNC") {
            own = false;  // the game already lights Logitech gear itself (through the proxy)
            logitechNote_ = s->game + " lights them itself";
        }
    if (own && !tracker_.Active())
        for (const GameStatus& g : games_)
            if (g.profile && g.profile->kind == games::ProfileKind::VendorSdk &&
                std::string(g.profile->how).find("Logitech") != std::string::npos) {
                own = false;  // e.g. Battlefield 1 talks to G HUB directly
                logitechNote_ = g.game.name + " lights them through G HUB";
                break;
            }
    logitech_.Set(output_.fx, own);
}

std::wstring Controller::GameDir(const char* profileKey) const {
    const games::GameProfile* want = games::ProfileByKey(profileKey);
    if (!want) return L"";
    for (const auto& g : library_) {
        for (const auto& e : g.exeNames)
            if (games::FindProfile(e, "") == want) return g.dir;
        if (games::FindProfile("", Utf8(g.name)) == want) return g.dir;
    }
    return L"";
}

void Controller::RefreshFeedSettings() {
    const std::wstring rl = GameDir("rocketleague");
    if (!rl.empty()) feeds_.SetRocketLeaguePort(RocketLeagueStatsPort(rl));
}

void Controller::RescanLibrary() {
    if (libraryJob_.valid()) return;  // already scanning
    libraryJob_ = std::async(std::launch::async, ScanInstalledGames, prefs_.manualGames);
}

void Controller::AddManualGame(const std::wstring& exePath) {
    auto& m = prefs_.manualGames;
    for (const auto& g : m)
        if (_wcsicmp(g.c_str(), exePath.c_str()) == 0) return;
    m.push_back(exePath);
    LUMA_INFO("games list: added %s", Utf8(exePath).c_str());
    detector_.SetExtraGames(m);
    Changed();
    if (libraryJob_.valid()) libraryRescanPending_ = true;
    else RescanLibrary();
}

void Controller::RemoveManualGame(const std::wstring& exePath) {
    auto& m = prefs_.manualGames;
    m.erase(std::remove_if(m.begin(), m.end(),
                           [&](const std::wstring& g) { return _wcsicmp(g.c_str(), exePath.c_str()) == 0; }),
            m.end());
    detector_.SetExtraGames(m);
    library_.erase(std::remove_if(library_.begin(), library_.end(),
                                  [&](const InstalledGame& g) {
                                      return g.manual && _wcsicmp(g.exePath.c_str(), exePath.c_str()) == 0;
                                  }),
                   library_.end());
    Changed();
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
    feeds_.Stop();
    screen_.Stop();
    monitor_.Stop();
    logitech_.Stop();
    azoth_.Stop();
    ram_.Stop();
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

void Controller::RainbowLook(fx::Params* p) const {
    p->hueStart = prefs_.rainbowHueStart;
    p->hueSpan = prefs_.rainbowHueSpan;
    p->saturation = prefs_.rainbowSaturation;
    p->spread = prefs_.rainbowSpread;
}

void Controller::RememberManualColor() { RememberColor(prefs_.manualColor); }

void Controller::RememberColor(Rgb c) {
    auto& r = prefs_.recentColors;
    r.erase(std::remove(r.begin(), r.end(), c), r.end());
    r.insert(r.begin(), c);
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
        RainbowLook(&o.fx);
        o.fx.reverse = prefs_.effectReverse;
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
            if (s->hasEffect) {
                g.fx = s->effect;
            } else {
                g.fx.kind = s->flashHz > 0 ? fx::Kind::Strobe : fx::Kind::Static;
                g.fx.color1 = s->color;
                g.fx.speed = s->flashHz;
            }
            return g;
        }
        games::ScreenColors sc;
        if (const GameStatus* g = ScreenColorsGame(); g && screen_.Latest(&sc)) {
            Output o = lit((g->game.name + " - screen colors").c_str());
            o.fx.kind = fx::Kind::Gradient;  // left of the screen <-> right, around each fan
            o.fx.color1 = sc.left;
            o.fx.color2 = sc.right;
            o.fx.speed = 0;
            return o;
        }
        // A running game with its own color (Games List).
        for (const GameStatus& g : games_) {
            if (g.mode != GameMode::Color) continue;
            auto c = prefs_.gameColors.find(games::Normalize(g.game.name));
            Output o = lit((g.game.name + " - its color").c_str());
            o.fx.color1 = c == prefs_.gameColors.end() ? prefs_.manualColor : c->second;
            return o;
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
            RainbowLook(&r.fx);
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
    if (n == games::Normalize(g.name) || n == games::Normalize(stem) || n == games::Normalize(g.folder)) return true;
    const games::GameProfile* p = games::FindProfile(g.exe, g.name);  // built-in feeds use the profile title
    return p && n == games::Normalize(p->title);
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
        st.profile = games::FindProfile(g.exe, g.name);
        if (st.profile && st.profile->kind == games::ProfileKind::NotAGame) continue;  // e.g. DSX
        auto mode = prefs_.gameModes.find(games::Normalize(g.name));
        if (mode != prefs_.gameModes.end()) st.mode = mode->second;
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
            if (!auraPaused_) {
                HandBackLighting();
                handbackDoneAt_ = GetTickCount64() + kHandbackMs;
            }
        }
        return;
    }
    handbackDoneAt_ = 0;  // LumaBridge has the lights again
    if (!mirror_.IsRunning()) {
        Config c = cfg_;
        c.releaseControlOnShutdown = true;
        mirror_.Start(c, nullptr, "LumaBridge app", /*routeToApp=*/false);
        mirrorHz_ = c.maxUpdateHz;
    }
    mirror_.SetPattern(out.fx, cfg_.argbFans, out.fanTest);
}

void Controller::UpdateFeeds(uint64_t now) {
    bool rl = false, wt = false;
    for (const GameStatus& g : games_)
        if (g.profile) {
            rl |= g.profile->feed == games::Feed::RocketLeagueStats;
            wt |= g.profile->feed == games::Feed::WarThunderApi;
        }
    feeds_.SetRunning(rl, wt);
    struct {
        GameFeeds::Feed feed;
        const char* sdk;
        const char* game;
    } feeds[] = {
        {feeds_.Cs2(now), "Game State Integration", "Counter-Strike 2"},
        {feeds_.RocketLeague(now), "Stats API", "Rocket League"},
        {feeds_.WarThunder(now), "local status page", "War Thunder"},
    };
    for (int i = 0; i < 3; ++i) {
        if (feeds[i].feed.active) {
            tracker_.OnEffect(0, feeds[i].sdk, feeds[i].game, feeds[i].feed.effect, now);
        } else if (feedActive_[i]) {
            tracker_.OnRelease(0, feeds[i].sdk);  // out of the match: back to the idle choice
        }
        feedActive_[i] = feeds[i].feed.active;
    }
}

const Controller::GameStatus* Controller::ScreenColorsGame() const {
    if (tracker_.Active()) return nullptr;  // a game is lighting things itself
    for (const GameStatus& g : games_) {
        if (g.mode == GameMode::Screen) return &g;
        if (g.mode == GameMode::Idle || g.mode == GameMode::Color) continue;
        const bool builtIn = g.profile && g.profile->kind == games::ProfileKind::BuiltIn;
        if (prefs_.screenForUnsupported && !builtIn && !games::SupportsLighting(g.support)) return &g;
    }
    return nullptr;
}

uint64_t Controller::handbackMsLeft() const {
    const uint64_t now = GetTickCount64();
    return handbackDoneAt_ > now ? handbackDoneAt_ - now : 0;
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
    UpdateFeeds(now);
    tracker_.Prune(now, [](const Source&) { return false; });
    UpdateGames(now);
    // Screen colors runs only while a running game falls back to it.
    const bool wantScreen = ScreenColorsGame() != nullptr && !auraPaused_ && !prefs_.lightingStopped &&
                            prefs_.mode == Mode::Auto;
    if (wantScreen && !screen_.Running()) screen_.Start();
    if (!wantScreen && screen_.Running()) screen_.Stop();
    if (libraryJob_.valid() && libraryJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        library_ = libraryJob_.get();
        RefreshFeedSettings();
        if (libraryRescanPending_) {
            libraryRescanPending_ = false;
            RescanLibrary();
        }
    }

    Output next = Decide();
    if (!outputApplied_ || !next.SameLighting(output_)) {
        Apply(next);
        outputApplied_ = true;
    }
    output_ = next;
    UpdateLogitech();
    azoth_.Set(output_.fx, cfg_.auraCorrection.brightness, prefs_.azothKeyboard && !output_.stopped);
    ram_.Set(output_.fx, cfg_.auraCorrection.brightness, prefs_.ramLighting && !output_.stopped);

    if (dirty_ && now - dirtySince_ >= kSaveDebounceMs) {
        SaveAll(iniPath_, prefs_, cfg_);
        dirty_ = false;
    }
}

}  // namespace luma::app
