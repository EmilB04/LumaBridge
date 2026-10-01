#include "controller.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>

#include "armoury_crate.h"
#include "integrations.h"
#include "log.h"
#include "usb_aura.h"
#include "azoth_protocol.h"
#include "dualsense_protocol.h"
#include "vendor_detect.h"
#include "setup_hardware.h"
#include "device_catalog.h"
#include "logitech_hidpp.h"
#include "ghub_settings.h"
#include "device_sleep.h"
#include "lighting_identity.h"

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
    feeds_.SetForzaPort(prefs_.forzaPort);
    feeds_.SetF1Port(prefs_.f1Port);
    feeds_.SetUdpPort(GameFeeds::kBeamNg, prefs_.beamngPort);
    feeds_.SetUdpPort(GameFeeds::kDirt, prefs_.dirtPort);
    feeds_.SetUdpPort(GameFeeds::kAms2, prefs_.ams2Port);
    feeds_.SetUdpPort(GameFeeds::kXPlane, prefs_.xplanePort);
    feeds_.Start();
    monitor_.Start(prefs_.lhmPort);
    if (prefs_.logitechDevices) logitech_.Start(AppDirectory() + L"\\integrations\\LumaBridge_x64.dll");
    azoth_.SetOled(prefs_.azothOled);
    azoth_.Start();  // Keep USB status current even while lighting and OLED control are off.
    if (prefs_.dualsenseController) dualsense_.Start();
    pad_.SetMapping(prefs_.padMapping);
    if (prefs_.padInput) pad_.Start();
    openRgb_.Start(static_cast<uint16_t>(prefs_.openRgbPort));
    lampArray_.Start();
    logitechInputAt_ = azothInputAt_ = GetTickCount64();  // awake at start
    hardware_.Start();
    return true;
}

void Controller::SetRamEnabled(bool on) {
    prefs_.ramLighting = on;

    Changed();
}

void Controller::OnDeviceInput(uint16_t vid, uint16_t pid) {
    const uint64_t now = GetTickCount64();
    if (vid == hidpp::kVendor) logitechInputAt_ = now;
    if (vid == azoth::kVendor && (pid == azoth::Product(azoth::Link::Wired) || pid == azoth::Product(azoth::Link::Wireless)))
        azothInputAt_ = now;
}

uint64_t Controller::LogitechSleepMs(bool dynamicActive) const {
    // G HUB's own setting decides whether the mouse's lighting turns off; LumaBridge's decides when.
    if (!prefs_.logitechSleep || !presence_.ghubSleep.value_or(true)) return 0;
    if (dynamicActive && prefs_.logitechSleepIgnoreDynamic) return 0;
    return static_cast<uint64_t>(std::max(10, prefs_.logitechSleepSec)) * 1000;
}

uint64_t Controller::AzothSleepMs(bool dynamicActive) const {
    if (!prefs_.azothSleep) return 0;
    if (dynamicActive && prefs_.azothSleepIgnoreDynamic) return 0;
    return static_cast<uint64_t>(std::max(10, prefs_.azothSleepSec)) * 1000;
}

void Controller::SetLampArrayEnabled(bool on) {
    prefs_.lampArray = on;
    lampArray_.Set(DeviceEffect(device::kOther), 0, false, {});
    lampArray_.Start();  // discovery continues while lighting is off
    Changed();
}

bool Controller::LampArrayDefaultOn(const LampArrayDevice& d) const {
    if (!d.problem.empty()) return false;
    if (d.vid == azoth::kVendor) {
        if (d.pid == azoth::Product(azoth::Link::Wired) || d.pid == azoth::Product(azoth::Link::Wireless))
            return !(prefs_.azothKeyboard &&
                     (azoth_.state() == AzothOutput::State::Active || azoth_.state() == AzothOutput::State::Released));
        for (uint16_t pid : aurausb::MainboardProductIds())
            if (d.pid == pid && auraStatus().connected) return false;
    }
    if (d.vid == hidpp::kVendor && prefs_.logitechDevices &&
        lighting::LogitechCovered(logitech_.sdkAvailable(), logitech_.directName(), d.name)) return false;
    return true;
}

bool Controller::LampArrayOn(const LampArrayDevice& d) const {
    if (!d.problem.empty()) return false;
    return lighting::Enabled(prefs_.lampArrayDevices, d.id, d.name, LampArrayDefaultOn(d));
}

void Controller::SetOpenRgbEnabled(bool on) {
    prefs_.openRgb = on;
    openRgb_.Set(DeviceEffect(device::kOther), 0, false, {});
    openRgb_.Start(static_cast<uint16_t>(prefs_.openRgbPort));
    Changed();
}

bool Controller::OpenRgbDefaultOn(const OpenRgbDevice& d) const {
    const lighting::NativeConnections available{
        auraStatus().connected,
        prefs_.logitechDevices && lighting::LogitechCovered(logitech_.sdkAvailable(), logitech_.directName(), d.name),
        prefs_.azothKeyboard && (azoth_.state() == AzothOutput::State::Active ||
                                azoth_.state() == AzothOutput::State::Released),
        prefs_.ramLighting && hardware_.sticks() > 0 &&
            (hardware_.ramState() == HardwareHelper::RamState::Active || hardware_.ramState() == HardwareHelper::RamState::Released),
        prefs_.nzxtLighting && kraken_.lightingActive(),
        prefs_.dualsenseController && (dualsense_.state() == DualSenseOutput::State::Active ||
                                      dualsense_.state() == DualSenseOutput::State::Connecting ||
                                      dualsense_.state() == DualSenseOutput::State::Released)};
    if (prefs_.lampArray && !DeviceNative(prefs_, device::kOther))
        for (const auto& lamp : lampArray_.devices())
            if (LampArrayOn(lamp) && lighting::SameLocation(d.location, lamp.path)) return false;
    return !lighting::PreferNative(d.type, d.name + " " + d.vendor, available);
}

bool Controller::OpenRgbOn(const OpenRgbDevice& d) const {
    return lighting::Enabled(prefs_.openRgbDevices, d.id, d.name, OpenRgbDefaultOn(d));
}

void Controller::SetOpenRgbPort(int port) {
    if (port < 1 || port > 65535 || port == prefs_.openRgbPort) return;
    openRgb_.Stop();
    prefs_.openRgbPort = port;
    openRgb_.Start(static_cast<uint16_t>(port));
    Changed();
}

void Controller::SetAzothEnabled(bool on) {
    prefs_.azothKeyboard = on;
    azoth_.Start();
    Changed();
}

void Controller::SetAzothOled(azoth::OledSettings settings) {
    prefs_.azothOled = azoth::NormalizeOled(settings);
    azoth_.SetOled(prefs_.azothOled);
    azoth_.Start();
    Changed();
}

void Controller::SetDualSenseEnabled(bool on) {
    prefs_.dualsenseController = on;
    if (on) dualsense_.Start();
    else dualsense_.Stop();
    Changed();
}

void Controller::SetPadInputEnabled(bool on) {
    prefs_.padInput = on;
    if (on) pad_.Start();
    else pad_.Stop();  // lets go of any key it holds
    Changed();
}

void Controller::SetLogitechEnabled(bool on) {
    prefs_.logitechDevices = on;
    if (on) logitech_.Start(AppDirectory() + L"\\integrations\\LumaBridge_x64.dll");
    else logitech_.Stop();  // G HUB takes its profile back
    Changed();
}

void Controller::UpdateLogitech() {
    // Not used for a while: its lighting fades out, then nothing goes to it (device_sleep.h),
    // unless it's showing a dynamic effect the owner asked never to interrupt.
    const uint64_t now = GetTickCount64();
    const fx::Params effect = DeviceEffect(device::kMouse);
    const bool dynamic = effect.kind != fx::Kind::Static;
    const double awake = sleep::Level(now, logitechInputAt_, LogitechSleepMs(dynamic));
    const bool asleep = sleep::Asleep(now, logitechInputAt_, LogitechSleepMs(dynamic));
    if (asleep != logitechAsleep_) LUMA_INFO("Logitech devices: %s", asleep ? "asleep (not used for a while)" : "awake");
    logitechAsleep_ = asleep;
    logitech_.SetAsleep(asleep);
    if (!prefs_.logitechDevices) {
        logitechNote_ = "Turned off";
        return;
    }
    bool own = !output_.stopped;
    logitechNote_ = own ? "" : "LumaBridge isn't controlling the lights - G HUB has them";
    if (own && DeviceNative(prefs_, device::kMouse)) {
        own = false;
        logitechNote_ = "Handed back to G HUB";
    }
    if (own && prefs_.logitechForce) {
        logitech_.Set(effect, cfg_.auraCorrection.brightness * DeviceBrightness(prefs_, device::kMouse) * awake, true,
                      output_.game);
        return;  // kept with LumaBridge even while a game lights Logitech gear
    }
    if (own && logitech_.sdkAvailable())
        if (auto s = tracker_.Active(); s && s->sdk == "Logitech LIGHTSYNC") {
            own = false;  // the game already lights Logitech gear itself (through the proxy)
            logitechNote_ = s->game + " lights them itself";
        }
    if (own && logitech_.sdkAvailable() && !tracker_.Active())
        for (const GameStatus& g : games_)
            if (g.profile && g.profile->kind == games::ProfileKind::VendorSdk &&
                std::string(g.profile->how).find("Logitech") != std::string::npos) {
                own = false;  // e.g. Battlefield 2042 lights them through G HUB (LIGHTSYNC)
                logitechNote_ = g.game.name + " lights them through G HUB";
                break;
            }
    logitech_.Set(effect, cfg_.auraCorrection.brightness * DeviceBrightness(prefs_, device::kMouse) * awake, own,
                  output_.game);
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
    feeds_.SetForzaPort(prefs_.forzaPort);
    feeds_.SetF1Port(prefs_.f1Port);
    feeds_.SetUdpPort(GameFeeds::kBeamNg, prefs_.beamngPort);
    feeds_.SetUdpPort(GameFeeds::kDirt, prefs_.dirtPort);
    feeds_.SetUdpPort(GameFeeds::kAms2, prefs_.ams2Port);
    feeds_.SetUdpPort(GameFeeds::kXPlane, prefs_.xplanePort);
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

void Controller::RescanPresence() {
    if (presenceJob_.valid()) return;  // already scanning
    presenceAt_ = GetTickCount64();
    presenceJob_ = std::async(std::launch::async, [] {
        Presence p;
        p.inventory = inventory::ScanPresentDevices();
        p.azoth = UsbDevicePresent(azoth::kVendor, {azoth::Product(azoth::Link::Wired), azoth::Product(azoth::Link::Wireless)});
        p.dualsense = UsbDevicePresent(dualsense::kVendor, {dualsense::kProductStandard, dualsense::kProductEdge});
        for (const auto& d : p.inventory.devices)
            if (d.vid == dualsense::kVendor && (d.pid == dualsense::kProductStandard || d.pid == dualsense::kProductEdge))
                p.dualsense = true;
        p.logitech = ScanLogitechDevices();
        p.ghubSleep = GHubTurnsOffOnInactivity();
        p.usb = UsbDevices();
        for (const auto& k : nzxt::FindByName()) p.krakens.emplace_back(k.vid, k.pid);
        p.scanned = true;
        LUMA_INFO("hardware inventory: %d present device nodes; Windows error %lu",
                  static_cast<int>(p.inventory.devices.size()), p.inventory.error);
        return p;
    });
}

void Controller::RescanDevices() {
    azoth_.Rescan();
    RescanPresence();
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
    dualsense_.Stop();
    pad_.Stop();
    openRgb_.Stop();
    kraken_.Stop();
    lampArray_.Stop();
    hardware_.Stop();
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
    pad_.SetMapping(prefs_.padMapping);
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

fx::Params Controller::DeviceEffect(const Output& out, const std::string& id) const {
    fx::Params p = out.For(id);
    if (DeviceReversed(prefs_, id)) p.reverse = !p.reverse;
    return p;
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
        o.fx = ToParams(MainLook(prefs_));
        for (const auto& [id, d] : prefs_.deviceLighting)
            if (d.own) o.devices[id] = ToParams(d.look);
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
            g.game = true;
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
            if (gs.profile && gs.profile->blocked)
                idle = gs.game.name + " - lights Logitech gear through G HUB";
            else
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

// The fans and the motherboard share one ASUS controller: they go back to Armoury Crate
// together.
bool Controller::AuraNative() const {
    return DeviceNative(prefs_, device::kFans) || DeviceNative(prefs_, device::kBoard);
}

void Controller::Apply(const Output& out) {
    auraNativeApplied_ = AuraNative();
    if (out.stopped || auraNativeApplied_) {
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
    const fx::Params board = DeviceEffect(out, device::kBoard);
    mirror_.SetPattern(DeviceEffect(out, device::kFans), cfg_.argbFans, out.fanTest, &board);
    mirror_.SetLevels(DeviceBrightness(prefs_, device::kFans), DeviceBrightness(prefs_, device::kBoard));
}

void Controller::UpdateFeeds(uint64_t now) {
    GameFeeds::Running running;
    for (const GameStatus& g : games_)
        if (g.profile) {
            running.rocketLeague |= g.profile->feed == games::Feed::RocketLeagueStats;
            running.warThunder |= g.profile->feed == games::Feed::WarThunderApi;
            running.league |= g.profile->feed == games::Feed::LeagueLiveClient;
            running.forza |= g.profile->feed == games::Feed::ForzaDataOut;
            running.flightSim |= g.profile->feed == games::Feed::FlightSimConnect;
            running.dcs |= g.profile->feed == games::Feed::DcsExport;
            running.f1 |= g.profile->feed == games::Feed::F1Telemetry;
            running.beamng |= g.profile->feed == games::Feed::BeamNgOutGauge;
            running.dirt |= g.profile->feed == games::Feed::DirtRallyUdp;
            running.ams2 |= g.profile->feed == games::Feed::Ams2Udp;
            running.xplane |= g.profile->feed == games::Feed::XPlaneUdp;
            running.elite |= g.profile->feed == games::Feed::EliteStatus;
        }
    feeds_.SetRunning(running);
    struct {
        GameFeeds::Feed feed;
        const char* sdk;
        const char* game;
    } feeds[] = {
        {feeds_.Cs2(now), "Game State Integration", "Counter-Strike 2"},
        {feeds_.RocketLeague(now), "Stats API", "Rocket League"},
        {feeds_.WarThunder(now), "local status page", "War Thunder"},
        {feeds_.Dota2(now), "Game State Integration", "Dota 2"},
        {feeds_.League(now), "Live Client Data API", "League of Legends"},
        {feeds_.Forza(now), "Data Out telemetry", "Forza"},
        {feeds_.FlightSim(now), "SimConnect", "Microsoft Flight Simulator"},
        {feeds_.Dcs(now), "export script", "DCS World"},
        {feeds_.F1(now), "UDP telemetry", "F1"},
        {feeds_.BeamNg(now), "OutGauge", "BeamNG.drive"},
        {feeds_.Dirt(now), "UDP telemetry", "DiRT Rally"},
        {feeds_.Ams2(now), "UDP telemetry", "Automobilista 2"},
        {feeds_.XPlane(now), "UDP data output", "X-Plane"},
        {feeds_.Elite(now), "Status.json", "Elite Dangerous"},
    };
    static_assert(sizeof(feeds) / sizeof(feeds[0]) == sizeof(feedActive_) / sizeof(feedActive_[0]), "one flag per feed");
    for (size_t i = 0; i < std::size(feeds); ++i) {
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
        // Its own lighting goes to Logitech gear through G HUB (anti-cheat keeps LumaBridge
        // out): the idle choice, not the screen, unless picked on the game's page.
        if (g.profile && g.profile->blocked) continue;
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
    if (presenceJob_.valid() && presenceJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        Presence fresh = presenceJob_.get();
        // Once per change: what NZXT has on USB, and which AIO was recognised (for bug reports).
        std::string nzxtIds;
        char b[24];
        for (const auto& [vid, pid] : fresh.usb)
            if (vid == nzxt::kVid) snprintf(b, sizeof b, " %04X", pid), nzxtIds += b;
        for (const auto& [vid, pid] : fresh.krakens) snprintf(b, sizeof b, " named:%04X:%04X", vid, pid), nzxtIds += b;
        const catalog::AioModel* aio = fresh.Aio();
        const std::string summary = nzxtIds + (aio ? std::string(" -> ") + aio->name : std::string(" -> no AIO"));
        if (summary != presenceLogged_) {
            LUMA_INFO("USB: %d device(s); NZXT:%s", static_cast<int>(fresh.usb.size()), summary.c_str());
            presenceLogged_ = summary;
        }
        presence_ = std::move(fresh);
    }
    {
        // An NZXT Kraken on USB: read its status while it's there (read only; CAM keeps its lights).
        const catalog::AioModel* aio = presence_.Aio();
        if (aio && aio->vid == nzxt::kVid && !kraken_.running()) kraken_.Start(aio->pid, aio->lcd);
        if (!(aio && aio->vid == nzxt::kVid) && kraken_.running()) kraken_.Stop();
    }
    if (now - presenceAt_ > 60000) RescanPresence();  // devices plugged in or out
    if (libraryJob_.valid() && libraryJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        library_ = libraryJob_.get();
        RefreshFeedSettings();
        if (libraryRescanPending_) {
            libraryRescanPending_ = false;
            RescanLibrary();
        }
    }

    // Once: how the firmware names the memory slots, and where LumaBridge draws each stick.
    if (!memoryLogged_) {
        const auto snap = monitor_.Snapshot();
        if (!snap.smbios.memory.empty()) {
            memoryLogged_ = true;
            const SetupHardware hw = DetectSetup(snap.smbios);
            for (const auto& m : snap.smbios.memory)
                LUMA_INFO("memory: \"%s\" (bank \"%s\"): %s %s", m.slot.c_str(), m.bank.c_str(), m.manufacturer.c_str(),
                          m.part.c_str());
            LUMA_INFO("memory: slots %s", hw.slotsKnown ? "read from the firmware" : "not understood - drawn as a guess");
        }
    }
    Output next = Decide();
    // One clock for every device: an effect keeps its start while it runs (colors may change),
    // so fans, board, keyboard and mouse render the same moment of it. A source that sets its
    // own start (a bomb's beat, from the plant) keeps it.
    {
        auto stamp = [&](fx::Params& p, const fx::Params& before) {
            if (p.epoch) return;
            p.epoch = !output_.stopped && before.epoch && before.kind == p.kind && before.speed == p.speed ? before.epoch : now;
        };
        stamp(next.fx, output_.fx);
        for (auto& [id, p] : next.devices) stamp(p, output_.For(id));
    }
    if (!outputApplied_ || !next.SameLighting(output_) || AuraNative() != auraNativeApplied_) {
        Apply(next);
        outputApplied_ = true;
    }
    output_ = next;
    UpdateLogitech();
    {
        std::vector<std::string> skip;
        for (const LampArrayDevice& d : lampArray_.devices())
            if (!LampArrayOn(d)) skip.push_back(d.id);
        lampArray_.Set(DeviceEffect(device::kOther), cfg_.auraCorrection.brightness * DeviceBrightness(prefs_, device::kOther),
                       prefs_.lampArray && !output_.stopped && !DeviceNative(prefs_, device::kOther), skip);
    }
    {
        std::vector<std::string> skip;
        for (const OpenRgbDevice& d : openRgb_.devices())
            if (!OpenRgbOn(d)) skip.push_back(d.id);
        openRgb_.Set(DeviceEffect(device::kOther), cfg_.auraCorrection.brightness * DeviceBrightness(prefs_, device::kOther),
                     prefs_.openRgb && !output_.stopped && !DeviceNative(prefs_, device::kOther), skip);
    }
    {
        const uint64_t now = GetTickCount64();
        const fx::Params keyboardEffect = DeviceEffect(device::kKeyboard);
        const bool dynamic = keyboardEffect.kind != fx::Kind::Static;
        const bool asleep = sleep::Asleep(now, azothInputAt_, AzothSleepMs(dynamic));
        if (asleep != azothAsleep_) LUMA_INFO("ROG Azoth: %s", asleep ? "asleep (not used for a while)" : "awake");
        azothAsleep_ = asleep;
        azoth_.SetAsleep(asleep);
        azoth_.Set(keyboardEffect,
                   cfg_.auraCorrection.brightness * DeviceBrightness(prefs_, device::kKeyboard) *
                       sleep::Level(now, azothInputAt_, AzothSleepMs(dynamic)),
                   prefs_.azothKeyboard && !output_.stopped && !DeviceNative(prefs_, device::kKeyboard));
    }
    kraken_.SetLighting(DeviceEffect(device::kOther),
                       cfg_.auraCorrection.brightness * DeviceBrightness(prefs_, device::kOther),
                       prefs_.nzxtLighting && !output_.stopped && !DeviceNative(prefs_, device::kOther));
    dualsense_.Set(DeviceEffect(device::kController),
                   cfg_.auraCorrection.brightness * DeviceBrightness(prefs_, device::kController),
                   prefs_.dualsenseController && !output_.stopped && !DeviceNative(prefs_, device::kController));
    hardware_.SetRam(DeviceEffect(device::kRam), cfg_.auraCorrection.brightness * DeviceBrightness(prefs_, device::kRam), prefs_.ramLighting,
                     !output_.stopped && !DeviceNative(prefs_, device::kRam), prefs_.ramRelease);
    if (now - sensorsPushedAt_ >= 500) {
        sensorsPushedAt_ = now;
        monitor_.SetBuiltInSensors(hardware_.Sensors(), hardware_.chip());
    }

    if (dirty_ && now - dirtySince_ >= kSaveDebounceMs) {
        SaveAll(iniPath_, prefs_, cfg_);
        dirty_ = false;
    }
}

}  // namespace luma::app
