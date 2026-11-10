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
#include "pc_layout.h"

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
    savedHandoffEnabled_ = prefs_.handoffColorEnabled;
    savedHandoffColor_ = prefs_.handoffColor;
    savedLightingStopped_ = prefs_.lightingStopped;
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
    feeds_.SetUdpPort(GameFeeds::kWrc, prefs_.wrcPort);
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
    nollie_.Start();  // discovery continues while lighting is off
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

uint64_t Controller::LogitechSleepMs(bool gameLighting) const {
    // G HUB's own setting decides whether the mouse's lighting turns off; LumaBridge's decides when.
    return sleep::Timeout(prefs_.logitechSleep && presence_.ghubSleep.value_or(true), prefs_.logitechSleepSec,
                          prefs_.logitechSleepIgnoreDynamic, gameLighting);
}

uint64_t Controller::AzothSleepMs(bool gameLighting) const {
    return sleep::Timeout(prefs_.azothSleep, prefs_.azothSleepSec, prefs_.azothSleepIgnoreDynamic, gameLighting);
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

void Controller::SetNollieEnabled(bool on) {
    prefs_.nollieLighting = on;
    Changed();
}

bool Controller::NollieOn(const NollieDevice& d) const {
    if (!d.problem.empty()) return false;
    auto it = prefs_.nollieDevices.find(d.id);
    return it == prefs_.nollieDevices.end() || it->second;
}

void Controller::SetOpenRgbEnabled(bool on) {
    prefs_.openRgb = on;
    openRgb_.Set(DeviceEffect(device::kOther), 0, false, {});
    openRgb_.Start(static_cast<uint16_t>(prefs_.openRgbPort));
    Changed();
}

bool Controller::OpenRgbDefaultOn(const OpenRgbDevice& d) const {
    bool nollieNative = false;
    if (prefs_.nollieLighting)
        for (const NollieDevice& n : nollie_.devices()) nollieNative = nollieNative || NollieOn(n);
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
                                      dualsense_.state() == DualSenseOutput::State::Released),
        nollieNative};
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

games::BombCountdown Controller::AzothBombCountdown(uint64_t now) const {
    const auto live = feeds_.Cs2Bomb(now, prefs_.azothOled.cs2BombSeconds);
    if (live.active) return output_.stopped ? games::BombCountdown{} : live;
    return games::TestBombCountdown(now, azothBombTestUntil_);
}

bool Controller::TestAzothBombCountdown() {
    if (!prefs_.azothOled.enabled || !prefs_.azothOled.cs2BombTimer || !prefs_.azothOled.direct ||
        azoth_.connection() != azoth::Connection::Wired || azothAsleep_ || azoth_.stuck() ||
        feeds_.Cs2Bomb(GetTickCount64(), prefs_.azothOled.cs2BombSeconds).active) return false;
    azothBombTestUntil_ = GetTickCount64() + 10000;
    azoth_.ReapplyOled();
    LUMA_INFO("ROG Azoth: starting a 10-second bomb display test");
    return true;
}

void Controller::SetAzothOled(azoth::OledSettings settings) {
    prefs_.azothOled = azoth::NormalizeOled(settings);
    azoth_.SetOled(prefs_.azothOled);
    if (!prefs_.azothOled.enabled || !prefs_.azothOled.cs2BombTimer) CancelAzothBombTest();
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
    // unless an active game lighting feed is keeping it awake.
    const uint64_t now = GetTickCount64();
    const fx::Params effect = DeviceEffect(device::kMouse);
    const uint64_t timeout = IsHandoff(device::kMouse) ? 0 : LogitechSleepMs(output_.GameFor(device::kMouse) && output_.Controls(device::kMouse));
    const double awake = sleep::Level(now, logitechInputAt_, timeout);
    const bool asleep = sleep::Asleep(now, logitechInputAt_, timeout);
    if (asleep != logitechAsleep_) LUMA_INFO("Logitech devices: %s", asleep ? "asleep (not used for a while)" : "awake");
    logitechAsleep_ = asleep;
    if (!prefs_.logitechDevices) {
        logitechNote_ = "Turned off";
        return;
    }
    bool own = output_.Controls(device::kMouse);
    logitechNote_ = own ? "" : "LumaBridge isn't controlling the lights - G HUB has them";
    if (own && DeviceNative(prefs_, device::kMouse)) {
        own = false;
        logitechNote_ = "Handed back to G HUB";
    }
    if (own && prefs_.logitechForce) {
        logitech_.Set(effect, cfg_.auraCorrection.brightness * Level(device::kMouse) * awake, true,
                      output_.GameFor(device::kMouse), asleep);
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
    logitech_.Set(effect, cfg_.auraCorrection.brightness * Level(device::kMouse) * awake, own,
                  output_.GameFor(device::kMouse), asleep);
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
    feeds_.SetUdpPort(GameFeeds::kWrc, prefs_.wrcPort);
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
    presenceJob_ = std::async(std::launch::async, [paused = logitechProbePaused_, cached = presence_.logitech] {
        Presence p;
        p.inventory = inventory::ScanPresentDevices();
        p.azoth = UsbDevicePresent(azoth::kVendor, {azoth::Product(azoth::Link::Wired), azoth::Product(azoth::Link::Wireless)});
        p.dualsense = UsbDevicePresent(dualsense::kVendor, {dualsense::kProductStandard, dualsense::kProductEdge});
        for (const auto& d : p.inventory.devices)
            if (d.vid == dualsense::kVendor && (d.pid == dualsense::kProductStandard || d.pid == dualsense::kProductEdge))
                p.dualsense = true;
        // Name/feature queries send HID++ reports and can wake a wireless mouse. Keep
        // its last inventory while idle; cancel a scan already underway when fading starts.
        if (*paused) p.logitech = cached;
        else {
            p.logitech = ScanLogitechDevices([paused] { return paused->load(); });
            if (*paused) p.logitech = cached;
        }
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
    if (!output_.stopped && prefs_.handoffColorEnabled) PaintHandoffColor(800);
    prefs_.lightingStopped = true;
    Changed();
}

void Controller::Shutdown(bool handBack) {
    if (shutDown_) return;
    if (!handoffRecovery_ && !output_.stopped && prefs_.handoffColorEnabled) PaintHandoffColor(800);
    shutDown_ = true;
    if (dirty_ && !handoffRecovery_) SaveAll(iniPath_, prefs_, cfg_);
    // Ask every worker to leave before joining any of them. Slow network timeouts and
    // device I/O then unwind together rather than consuming a timeout per worker.
    feeds_.RequestStop(); screen_.RequestStop(); monitor_.RequestStop();
    logitech_.RequestStop(); azoth_.RequestStop(); dualsense_.RequestStop(); pad_.RequestStop();
    openRgb_.RequestStop(); kraken_.RequestStop(); lampArray_.RequestStop(); nollie_.RequestStop();
    mirror_.RequestStop();
    gameSense_.Stop();
    LUMA_INFO("shutdown: stopping game feeds"); feeds_.Stop();
    LUMA_INFO("shutdown: stopping capture and sensors"); screen_.Stop(); monitor_.Stop();
    LUMA_INFO("shutdown: Logitech"); logitech_.Stop();
    LUMA_INFO("shutdown: Azoth"); azoth_.Stop();
    LUMA_INFO("shutdown: DualSense"); dualsense_.Stop();
    LUMA_INFO("shutdown: controller input"); pad_.Stop();
    LUMA_INFO("shutdown: OpenRGB"); openRgb_.Stop();
    LUMA_INFO("shutdown: Kraken"); kraken_.Stop();
    LUMA_INFO("shutdown: LampArray"); lampArray_.Stop();
    LUMA_INFO("shutdown: Nollie"); nollie_.Stop();
    LUMA_INFO("shutdown: Aura");
    const bool wasControlling = mirror_.IsRunning();
    mirror_.Stop();
    LUMA_INFO("shutdown: releasing hardware helper");
    hardware_.Stop();  // close Aura's USB handle before the helper restores ASUS lighting
    // Exiting LumaBridge gives the lights back to Armoury Crate (not during a Windows
    // shutdown: the controller reloads Armoury Crate's saved effect at the next boot anyway).
    if (wasControlling && handBack && !prefs_.handoffColorEnabled) HandBackLighting();
    if (!handoffRecovery_) WriteConfigValue(iniPath_, L"App", L"Running", L"0");
    LUMA_INFO("LumaBridge app exiting");
}

void Controller::Changed() {
    if (mirror_.IsRunning() && cfg_.maxUpdateHz != mirrorHz_) mirror_.Stop();  // restarted by Tick
    mirror_.SetCorrection(cfg_.auraCorrection);
    mirror_.SetDisabledDevices(cfg_.auraDisabledDevices);
    pad_.SetMapping(prefs_.padMapping);
    outputApplied_ = false;  // re-apply manual color / effect edits right away
    // Only recovery-related edits bypass the regular debounced settings save.
    if (savedHandoffEnabled_ != prefs_.handoffColorEnabled) {
        WriteConfigValue(iniPath_, L"App", L"HandoffColorEnabled", prefs_.handoffColorEnabled ? L"1" : L"0");
        savedHandoffEnabled_ = prefs_.handoffColorEnabled;
    }
    if (savedHandoffColor_ != prefs_.handoffColor) {
        const std::string hex = nollie::hubdetail::Hex(prefs_.handoffColor);
        WriteConfigValue(iniPath_, L"App", L"HandoffColor", std::wstring(hex.begin(), hex.end()).c_str());
        savedHandoffColor_ = prefs_.handoffColor;
    }
    if (savedLightingStopped_ != prefs_.lightingStopped) {
        WriteConfigValue(iniPath_, L"App", L"LightingStopped", prefs_.lightingStopped ? L"1" : L"0");
        savedLightingStopped_ = prefs_.lightingStopped;
    }
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

Spot Controller::ArrangedSpot(const std::string& item) const {
    const Spot flat = SetupSpot(prefs_, item);
    if (!prefs_.lighting3d) return flat;
    // In the 3D view the arrangement is the desk and the case: the order follows what was moved
    // there, not the flat layout behind it.
    auto deskSpot = [&](const std::string& key) {
        auto it = prefs_.setupSpots.find(key);
        if (it != prefs_.setupSpots.end()) return it->second;
        Spot s;
        pc::ToSaved(pc::DefaultDeskSpot(key), &s.x, &s.y);
        return s;
    };
    static const std::map<std::string, std::string> desk = {
        {"keyboard", "desk:keyboard"}, {"mouse", "desk:mouse"},   {"controller", "desk:controller"}, {"headset", "desk:headset"},
        {"other0", "desk:other0"},     {"other1", "desk:other1"}, {"other2", "desk:other2"},         {"other3", "desk:other3"}};
    if (auto it = desk.find(item); it != desk.end()) return deskSpot(it->second);
    // Inside the case: where the case stands, the devices in their flat order around it.
    const Spot c = deskSpot("desk:case");
    return Spot{c.x + (flat.x - 0.5f) * 0.2f, c.y + (flat.y - 0.5f) * 0.2f};
}

std::string Controller::ZoneKey(const std::string& device) const {
    const Zone* z = ZoneOf(prefs_.zones, ZoneDevice(device));
    return z ? z->id : std::string();
}

double Controller::Level(const std::string& id) const {
    double v = DeviceBrightness(prefs_, id);
    if (const Zone* z = ZoneOf(prefs_.zones, ZoneDevice(id))) v *= z->brightness;
    return v;
}

fx::Params Controller::DeviceEffect(const Output& out, const std::string& id) const {
    if (IsHandoff(id)) {
        fx::Params final;
        final.color1 = prefs_.handoffColor;
        final.delivery = handoffToken_;
        return final;
    }
    const std::string who = ZoneDevice(id);
    // The device's own entry first, else (a Nollie channel or fan) its device's, else the main.
    fx::Params p = out.For(id);
    if (!out.devices.count(id) && out.devices.count(who)) p = out.devices.at(who);
    const Zone* zone = ZoneOf(prefs_.zones, who);
    bool reversed = DeviceReversed(prefs_, id);
    if (zone && zone->reverse) reversed = !reversed;
    if (reversed) p.reverse = !p.reverse;
    const std::string key = zone ? zone->id : std::string();
    if (fx::IsSetupEffect(p.kind)) {
        // Its place among the parts of its zone; a Nollie channel or fan counts from the hubs' first part.
        auto it = setupParts_.find(key);
        p.parts = it != setupParts_.end() ? it->second.count : 1;
        p.part = 0;
        if (it != setupParts_.end())
            for (const SetupPart& part : it->second.parts)
                if (part.id == who) p.part = part.first;
        return p;
    }
    // Your own lighting can travel across the zone's devices; a game's lights every device at once.
    const auto dir = static_cast<FlowDirection>(zone ? zone->flowDirection : prefs_.flowDirection);
    const float seconds = zone ? zone->flowSeconds : prefs_.flowSeconds;
    if (dir != FlowDirection::Off && !out.GameFor(who)) {
        std::map<std::string, Spot> at;
        for (const char* d : device::All())
            if (ZoneKey(d) == key)
                at[d] = FlowSpot(d, cfg_.argbFans.Fans(), [&](const std::string& item) { return ArrangedSpot(item); });
        // Anything not on the layout doesn't flow (Flowed at 0 would run it a whole flow early).
        const auto offsets = FlowOffsets(at, dir);
        if (auto it = offsets.find(who); it != offsets.end()) p = Flowed(p, it->second, seconds);
    }
    return p;
}

void Controller::UpdateSetupParts() {
    // Which devices are lit, and how many parts each one has.
    struct Candidate {
        const char* id;
        std::string name;
        int count;
    };
    std::vector<Candidate> lit;
    const auto aura = auraStatus();
    bool argb = false, board = false;
    for (const auto& d : aura.devices) {
        if (d.type == 0x00011000) argb = true;
        else board = true;
    }
    int nollieParts = 0;
    if (prefs_.nollieLighting)
        for (const NollieDevice& d : nollie_.devices()) {
            if (!NollieOn(d)) continue;
            std::vector<std::pair<int, nollie::Channel>> zones;
            for (const auto& [key, c] : prefs_.nollieChannels)
                if (key.rfind(d.id + ".", 0) == 0 && c.kind != nollie::Kind::Unused)
                    zones.emplace_back(std::atoi(key.c_str() + d.id.size() + 1), c);
            std::sort(zones.begin(), zones.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            int count = 0;
            nollie::TravelSteps(zones, &count);
            nollieParts += count;
        }
    const int sticks = hardware_.sticks();
    for (const char* id : device::All()) {
        if (DeviceNative(prefs_, id)) continue;
        const std::string d = id;
        int count = 0;
        if (d == device::kFans && aura.connected && argb) count = cfg_.argbFans.repeatPerFan ? cfg_.argbFans.Fans() : 1;
        if (d == device::kBoard && aura.connected && board) count = 1;
        if (d == device::kNollie) count = nollieParts;
        if (d == device::kRam && prefs_.ramLighting && hardware_.ramState() == HardwareHelper::RamState::Active)
            count = prefs_.setupRamApart ? sticks : 1;
        if (d == device::kMouse && prefs_.logitechDevices &&
            (logitech_.state() == LogitechOutput::State::Active || logitech_.sdkActive()))
            count = 1;
        if (d == device::kKeyboard && prefs_.azothKeyboard && azoth_.state() == AzothOutput::State::Active) count = 1;
        if (d == device::kController && prefs_.dualsenseController && dualsense_.state() == DualSenseOutput::State::Active)
            count = 1;
        if (d == device::kOther && ((prefs_.openRgb && !openRgb_.devices().empty()) || (prefs_.lampArray && !lampArray_.devices().empty())))
            count = 1;
        if (count > 0) lit.push_back({id, device::Name(id), count});
    }
    // Each zone (and Everything, "") has its parts in the order of its own arrangement.
    std::map<std::string, ZoneParts> next;
    std::vector<std::string> keys{""};
    for (const Zone& z : prefs_.zones) keys.push_back(z.id);
    for (const std::string& key : keys) {
        const Zone* zone = FindZone(prefs_.zones, key);
        std::vector<Candidate> mine;
        for (const Candidate& c : lit)
            if (ZoneKey(c.id) == key) mine.push_back(c);
        const auto dir = static_cast<FlowDirection>(zone ? zone->order : prefs_.setupOrder);
        std::map<std::string, Spot> at;
        for (const Candidate& c : mine)
            at[c.id] = FlowSpot(c.id, cfg_.argbFans.Fans(), [&](const std::string& item) { return ArrangedSpot(item); });
        const auto along = FlowOffsets(at, dir == FlowDirection::Off ? FlowDirection::LeftToRight : dir);
        std::stable_sort(mine.begin(), mine.end(), [&](const Candidate& a, const Candidate& b) {
            return along.at(a.id) < along.at(b.id);
        });
        ZoneParts zp;
        int first = 0;
        for (const Candidate& c : mine) {
            zp.parts.push_back({c.id, c.name, first, c.count});
            first += c.count;
        }
        zp.count = std::max(1, first);
        next[key] = zp;
    }
    // A part list that changed (a device came or went, memory sticks together or apart, a device
    // moved to another zone): the outputs that only take a new effect when the lighting
    // changes (Aura) get it again.
    bool same = next.size() == setupParts_.size();
    for (auto a = next.begin(), b = setupParts_.begin(); same && a != next.end(); ++a, ++b)
        same = a->first == b->first && a->second.count == b->second.count && a->second.parts.size() == b->second.parts.size() &&
               std::equal(a->second.parts.begin(), a->second.parts.end(), b->second.parts.begin(),
                          [](const SetupPart& x, const SetupPart& y) { return x.id == y.id && x.first == y.first && x.count == y.count; });
    if (!same) outputApplied_ = false;
    setupParts_ = next;
}

// A running game's lighting (its effect, the screen's colors, or the game's own color); false
// when none runs.
bool Controller::GameOutput(Output* out) const {
    auto lit = [](const std::string& label) {
        Output o;
        o.stopped = false;
        o.label = label;
        return o;
    };
    if (auto s = tracker_.Active()) {
        std::string name = s->game;
        for (const GameStatus& gs : games_)
            if (SourceBelongsTo(*s, gs.game)) name = gs.game.name;
        Output g = lit(name + " - " + s->sdk);
        g.game = true;
        if (s->hasEffect) {
            g.fx = s->effect;
        } else {
            g.fx.kind = s->flashHz > 0 ? fx::Kind::Strobe : fx::Kind::Static;
            g.fx.color1 = s->color;
            g.fx.speed = s->flashHz;
        }
        *out = g;
        return true;
    }
    games::ScreenColors sc;
    if (const GameStatus* g = ScreenColorsGame(); g && screen_.Latest(&sc)) {
        Output o = lit(g->game.name + " - screen colors");
        o.game = true;
        o.fx.kind = fx::Kind::Gradient;  // left of the screen <-> right, around each fan
        o.fx.color1 = sc.left;
        o.fx.color2 = sc.right;
        o.fx.speed = 0;
        *out = o;
        return true;
    }
    // A running game with its own color (Games List).
    for (const GameStatus& g : games_) {
        if (g.mode != GameMode::Color) continue;
        auto c = prefs_.gameColors.find(games::Normalize(g.game.name));
        Output o = lit(g.game.name + " - its color");
        o.fx.color1 = c == prefs_.gameColors.end() ? prefs_.manualColor : c->second;
        *out = o;
        return true;
    }
    return false;
}

// What a zone shows while it is manual (or auto, between games with its own look).
fx::Params Controller::ZoneParams(const Zone& z) const {
    fx::Params p;
    if (z.setupEffect) {
        p.kind = static_cast<fx::Kind>(z.setupEffect);
        p.color1 = z.setupColor1;
        p.color2 = z.setupColor2;
        p.speed = z.setupSpeed;
    } else {
        p = ToParams(z.look);
    }
    return p;
}

// Zones put their devices on a lighting of their own: manual ones always their look, auto ones
// a game's lighting while one runs and else what they show between games.
void Controller::ApplyZones(Output* o) const {
    if (prefs_.zones.empty() || auraPaused_ || prefs_.lightingStopped) return;
    if (o->stopped) {
        const auto outside = DevicesOutsideZones(prefs_.zones);
        if (outside.size() == device::All().size()) return;  // no zone owns a device
        o->released = outside;
        o->stopped = false;
    }
    Output game;
    const bool haveGame = GameOutput(&game);
    for (const Zone& z : prefs_.zones) {
        fx::Params p;
        bool forGame = false;
        if (z.manual) {
            p = ZoneParams(z);
        } else if (haveGame) {
            p = game.fx;
            forGame = true;
        } else if (z.idle == ZoneIdle::Rainbow) {
            p.kind = fx::Kind::RainbowWave;
            p.color1 = Rgb{170, 60, 255};
            p.speed = 0.1;
            RainbowLook(&p);
        } else if (z.idle == ZoneIdle::Off) {
            p.color1 = Rgb{};
        } else {
            p = ZoneParams(z);
        }
        for (const std::string& dev : z.devices) {
            o->devices[dev] = p;
            o->gameFor[dev] = forGame;
        }
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
        o.fx = ToParams(MainLook(prefs_));
        // A setup effect runs across every device in place of your lighting (their own looks too).
        if (fx::IsSetupEffect(static_cast<fx::Kind>(prefs_.setupEffect)) && prefs_.setupEffect) {
            o.fx = fx::Params{};
            o.fx.kind = static_cast<fx::Kind>(prefs_.setupEffect);
            o.fx.color1 = prefs_.setupColor1;
            o.fx.color2 = prefs_.setupColor2;
            o.fx.speed = prefs_.setupSpeed;
            return o;
        }
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

        Output game;
        if (GameOutput(&game)) return game;
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
    ApplyZones(&o);
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
    if ((!out.Controls(device::kFans) && !out.Controls(device::kBoard)) || auraNativeApplied_) {
        if (mirror_.IsRunning()) {
            LUMA_INFO("no longer controlling the lights - handing back to Armoury Crate");
            mirror_.Stop();
            // After a crash-loop pause LumaBridge never took the lights, so nothing to hand back.
            if (!auraPaused_) {
                if (!prefs_.handoffColorEnabled) {
                    HandBackLighting();
                    handbackDoneAt_ = GetTickCount64() + kHandbackMs;
                }
            }
        }
        return;
    }
    handbackDoneAt_ = 0;  // LumaBridge has the lights again
    if (!mirror_.IsRunning()) {
        Config c = cfg_;
        c.releaseControlOnShutdown = !prefs_.handoffColorEnabled;
        mirror_.Start(c, nullptr, "LumaBridge app", /*routeToApp=*/false);
        mirrorHz_ = c.maxUpdateHz;
    }
    auto disabled = cfg_.auraDisabledDevices;
    for (const auto& d : mirror_.GetStatus().devices) {
        const char* id = d.type == 0x00011000 ? device::kFans : device::kBoard;
        if (!out.Controls(id)) disabled.push_back(d.name);
    }
    mirror_.SetDisabledDevices(disabled);
    const fx::Params board = DeviceEffect(out, device::kBoard);
    mirror_.SetPattern(DeviceEffect(out, device::kFans), cfg_.argbFans, out.fanTest, &board);
    mirror_.SetLevels(Level(device::kFans), Level(device::kBoard));
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
            running.wrc |= g.profile->feed == games::Feed::WrcUdp;
            running.ac |= g.profile->feed == games::Feed::AcSharedMemory;
            running.iracing |= g.profile->feed == games::Feed::IRacingSdk;
            running.raceroom |= g.profile->feed == games::Feed::RaceRoomSharedMemory;
        }
    feeds_.SetRunning(running);
    // A feed several games share (OutGauge, the Codemasters telemetry, ...) carries the name of
    // the one running, so its lighting is matched to it on the Games pages.
    auto gameFor = [&](games::Feed feed, const char* fallback) {
        for (const GameStatus& g : games_)
            if (g.profile && g.profile->feed == feed) return g.game.name;
        return std::string(fallback);
    };
    using F = games::Feed;
    struct {
        GameFeeds::Feed feed;
        const char* sdk;
        std::string game;
    } feeds[] = {
        {feeds_.Cs2(now), "Game State Integration", "Counter-Strike 2"},
        {feeds_.RocketLeague(now), "Stats API", "Rocket League"},
        {feeds_.WarThunder(now), "local status page", "War Thunder"},
        {feeds_.Dota2(now), "Game State Integration", "Dota 2"},
        {feeds_.League(now), "Live Client Data API", "League of Legends"},
        {feeds_.Forza(now), "Data Out telemetry", gameFor(F::ForzaDataOut, "Forza")},
        {feeds_.FlightSim(now), "SimConnect", "Microsoft Flight Simulator"},
        {feeds_.Dcs(now), "export script", "DCS World"},
        {feeds_.F1(now), "UDP telemetry", gameFor(F::F1Telemetry, "F1")},
        {feeds_.BeamNg(now), "OutGauge", gameFor(F::BeamNgOutGauge, "BeamNG.drive")},
        {feeds_.Dirt(now), "Codemasters telemetry", gameFor(F::DirtRallyUdp, "DiRT Rally")},
        {feeds_.Ams2(now), "UDP telemetry", gameFor(F::Ams2Udp, "Automobilista 2")},
        {feeds_.XPlane(now), "UDP data output", "X-Plane"},
        {feeds_.Elite(now), "Status.json", "Elite Dangerous"},
        {feeds_.Wrc(now), "WRC telemetry", "EA SPORTS WRC"},
        {feeds_.Ac(now), "shared memory telemetry", gameFor(F::AcSharedMemory, "Assetto Corsa")},
        {feeds_.IRacing(now), "iRacing SDK", "iRacing"},
        {feeds_.RaceRoom(now), "R3E shared memory", "RaceRoom Racing Experience"},
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

void Controller::OnSystemSuspend() {
    if (systemSuspended_) return;
    systemSuspended_ = true;
    suspendedTicks_ = 0;
    *logitechProbePaused_ = true;
    LUMA_INFO("System sleep: turning off fan and Logitech lighting, then pausing updates");
    // Ask both workers first so they can finish together. Windows gives this handler
    // about two seconds; never wait indefinitely for a disappearing USB device.
    mirror_.SetSystemSuspended(true);
    logitech_.SetSystemSuspended(true);
    if (!mirror_.WaitForSuspend(750)) LUMA_WARN("System sleep: Aura worker didn't finish in time");
    if (!logitech_.WaitForSuspend(750)) LUMA_WARN("System sleep: Logitech worker didn't finish in time");
}

void Controller::OnSystemResume() {
    if (!systemSuspended_) {
        OnHardwareChanged();
        return;
    }
    LUMA_INFO("System resume: restoring fan and Logitech lighting");
    systemSuspended_ = false;
    *logitechProbePaused_ = false;
    logitechInputAt_ = GetTickCount64();
    logitech_.SetAsleep(false);
    mirror_.SetSystemSuspended(false);
    logitech_.SetSystemSuspended(false);
    outputApplied_ = false;
    OnHardwareChanged();
}

void Controller::Tick() {
    if (systemSuspended_) {
        // Ticks only run while the PC is awake. Half a minute of them without Windows reporting
        // the resume (a cancelled sleep, a missed message): resume anyway, rather than leave the
        // lights dark.
        if (++suspendedTicks_ < 600) return;
        LUMA_WARN("System resume wasn't reported - restoring the lighting anyway");
        OnSystemResume();
    }
    const uint64_t now = GetTickCount64();
    const uint64_t mouseTimeout = LogitechSleepMs(output_.GameFor(device::kMouse) && output_.Controls(device::kMouse));
    *logitechProbePaused_ = mouseTimeout && now >= logitechInputAt_ && now - logitechInputAt_ >= mouseTimeout;

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
                            NeedsGameCapture(prefs_.mode == Mode::Auto, prefs_.zones);
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
    UpdateSetupParts();
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
    SendOutputs();
    // Something (Armoury Crate, when an ASUS keyboard is plugged in by cable) can start the
    // service again every few seconds. Each time it runs it takes the motherboard's channels
    // out of direct mode, and LumaBridge's frames are then ignored without an error (black
    // fans and board), so once the helper has stopped it again, take them back.
    if (const uint32_t stops = hardware_.asusStops(); stops != asusStops_) {
        asusStops_ = stops;
        if (mirror_.IsRunning()) mirror_.Reclaim();
    }
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

namespace luma::app {
void Controller::SendOutputs() {
    if (!handoffActive_ && output_.Controls(device::kOther) && !DeviceNative(prefs_, device::kOther)) {
        lampArray_.KeepLastColor(false);
        openRgb_.KeepLastColor(false);
    }
    UpdateLogitech();
    {
        std::vector<std::string> skip;
        for (const LampArrayDevice& d : lampArray_.devices())
            if (!LampArrayOn(d)) skip.push_back(d.id);
        lampArray_.Set(DeviceEffect(device::kOther), cfg_.auraCorrection.brightness * Level(device::kOther),
                       prefs_.lampArray && output_.Controls(device::kOther) && !DeviceNative(prefs_, device::kOther), skip);
    }
    {
        std::vector<std::string> skip;
        std::map<std::string, NollieChannelPlan> plans;
        std::map<std::string, NollieHubPlan> hubPlans;
        for (const NollieDevice& d : nollie_.devices())
            if (!NollieOn(d)) skip.push_back(d.id);
        // The channels of each hub in order, for an effect travelling through them: each fan on
        // its own is a step, a strip or fans sharing one pattern a step together.
        std::map<std::string, std::vector<std::pair<int, nollie::Channel>>> inUse;
        for (const auto& [key, channel] : prefs_.nollieChannels) {
            const size_t dot = key.rfind('.');
            if (dot != std::string::npos && channel.kind != nollie::Kind::Unused)
                inUse[key.substr(0, dot)].emplace_back(std::atoi(key.c_str() + dot + 1), channel);
        }
        std::map<std::string, std::map<int, int>> firstStep;  // hub, zone -> step
        std::map<std::string, int> steps;
        for (auto& [id, zones] : inUse) {
            std::sort(zones.begin(), zones.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [zone, step] : nollie::TravelSteps(zones, &steps[id])) firstStep[id][zone] = step;
        }
        // Where each hub's parts start among the Nollie parts (setup effects), in the order
        // UpdateSetupParts counts them.
        std::map<std::string, int> hubBase;
        {
            int base = 0;
            for (const NollieDevice& d : nollie_.devices()) {
                if (!NollieOn(d)) continue;
                hubBase[d.id] = base;
                base += steps[d.id];
            }
        }
        auto own = [&](const std::string& id) {
            auto it = prefs_.deviceLighting.find(id);
            return it != prefs_.deviceLighting.end() && it->second.own;
        };
        for (const auto& [key, channel] : prefs_.nollieChannels) {
            const size_t dot = key.rfind('.');
            const std::string hubId = dot == std::string::npos ? key : key.substr(0, dot);
            const int zone = dot == std::string::npos ? 0 : std::atoi(key.c_str() + dot + 1);
            const std::string channelId = nollie::LightingId(hubId, zone);
            const auto hubSettings = prefs_.nollieHubs.find(hubId);
            const nollie::Hub hub = hubSettings != prefs_.nollieHubs.end() ? hubSettings->second : nollie::Hub{};
            NollieChannelPlan plan;
            plan.channel = channel;
            // Without a look of its own, a channel shows the Nollie fans' (Lighting page), which
            // without one of its own is the main lighting, like every other device.
            const std::string baseId = own(channelId) ? channelId : std::string(device::kNollie);
            plan.lit = !DeviceNative(prefs_, channelId) && !DeviceNative(prefs_, device::kNollie);
            plan.test = key == nollieTest_;
            plan.bootTravelSeconds = hub.travel == nollie::Travel::Off ? 0 : hub.travelSeconds;
            const int parts = channel.Segments();
            // A sequence runs through the parts in the travel's order (first to last when the
            // effect doesn't travel). Games light every device at once, so not during one.
            const bool nollieGame = output_.GameFor(device::kNollie);
            const bool setupEffect = fx::IsSetupEffect(DeviceEffect(device::kNollie).kind) && !nollieGame;
            if (!IsHandoff(device::kNollie) && !IsHandoff(channelId) && !nollieGame && !setupEffect) plan.sequence = hub.sequence;  // a setup effect runs instead
            plan.parts = std::max(1, steps[hubId]);
            plan.firstPart = firstStep[hubId][zone];
            if (hub.travel == nollie::Travel::Backward) {
                plan.firstPart = plan.parts - plan.firstPart - parts;
                plan.backward = true;
            }
            plan.reverse = DeviceReversed(prefs_, channelId);
            if (!IsHandoff(device::kNollie) && !IsHandoff(channelId) && !nollieGame) plan.gradient = hub.gradient;  // games keep their own colors
            plan.gradientFirst = firstStep[hubId][zone];
            for (int i = 0; i < parts; ++i) {
                // A fan on its own: its own look if it has one, else the channel's.
                const std::string fanId = nollie::FanLightingId(hubId, zone, i);
                const std::string lookId = parts > 1 && own(fanId) ? fanId : baseId;
                const nollie::TravelPlace place =
                    nollie::Place(hub.travel, hub.smooth, firstStep[hubId][zone] + i, steps[hubId], hub.travelSeconds);
                NollieSegmentPlan seg;
                seg.effect = IsHandoff(channelId) ? DeviceEffect(channelId) : DeviceEffect(lookId);
                // Setup effects: every fan and strip a part of its own, after the hubs before it.
                if (fx::IsSetupEffect(seg.effect.kind)) seg.effect.part += hubBase[hubId] + firstStep[hubId][zone] + i;
                if (lookId != channelId && DeviceReversed(prefs_, channelId)) seg.effect.reverse = !seg.effect.reverse;
                // Games light every device at once; your own lighting travels (only moving effects
                // with a shared start do: what Flowed moves).
                if (hub.travel != nollie::Travel::Off && !nollieGame) {
                    const fx::Params before = seg.effect;
                    seg.effect = Flowed(seg.effect, place.offset, hub.travelSeconds);
                    if (fx::IsAnimated(before) && before.epoch && before.kind != fx::Kind::Beat && !fx::IsSetupEffect(before.kind))
                        seg.lag = place.lag;
                }
                seg.brightness = cfg_.auraCorrection.brightness * Level(device::kNollie) *
                                 DeviceBrightness(prefs_, channelId) *
                                 (parts > 1 ? DeviceBrightness(prefs_, fanId) : 1.f);
                seg.bootEffect = nollie::BootEffect(hub, firstStep[hubId][zone] + i, plan.parts);
                if (DeviceReversed(prefs_, channelId)) seg.bootEffect.reverse = !seg.bootEffect.reverse;
                seg.bootOffset = place.offset;
                seg.bootLag = hub.travel == nollie::Travel::Off || fx::IsSetupEffect(seg.bootEffect.kind) ? 0 : place.lag;
                plan.segments.push_back(seg);
            }
            plans[key] = plan;
            NollieHubPlan& hp = hubPlans[hubId];
            hp.refreshHz = hub.refreshHz;
            if (hub.boot && plan.lit) hp.bootSeconds = hub.bootSeconds;
        }
        // The same color calibration as the Aura fans (Settings), so the same color looks the
        // same on both; brightness is in the plans already.
        ColorCorrection cc = cfg_.auraCorrection;
        cc.brightness = 1.0;
        nollie_.Set(prefs_.nollieLighting && output_.Controls(device::kNollie), plans, skip, hubPlans, cc);
    }
    {
        std::vector<std::string> skip;
        for (const OpenRgbDevice& d : openRgb_.devices())
            if (!OpenRgbOn(d)) skip.push_back(d.id);
        openRgb_.Set(DeviceEffect(device::kOther), cfg_.auraCorrection.brightness * Level(device::kOther),
                     prefs_.openRgb && output_.Controls(device::kOther) && !DeviceNative(prefs_, device::kOther), skip);
    }
    {
        const uint64_t now = GetTickCount64();
        const fx::Params keyboardEffect = DeviceEffect(device::kKeyboard);
        const uint64_t timeout = IsHandoff(device::kKeyboard) ? 0 : AzothSleepMs(output_.GameFor(device::kKeyboard) && output_.Controls(device::kKeyboard));
        const bool asleep = sleep::Asleep(now, azothInputAt_, timeout);
        if (asleep != azothAsleep_) LUMA_INFO("ROG Azoth: %s", asleep ? "asleep (not used for a while)" : "awake");
        azothAsleep_ = asleep;
        azoth_.SetBombCountdown(AzothBombCountdown(now));
        azoth_.Set(keyboardEffect,
                   cfg_.auraCorrection.brightness * Level(device::kKeyboard) *
                       sleep::Level(now, azothInputAt_, timeout),
                   prefs_.azothKeyboard && output_.Controls(device::kKeyboard) && !DeviceNative(prefs_, device::kKeyboard), asleep);
    }
    kraken_.SetLighting(DeviceEffect(device::kOther),
                       cfg_.auraCorrection.brightness * Level(device::kOther),
                       prefs_.nzxtLighting && output_.Controls(device::kOther) && !DeviceNative(prefs_, device::kOther));
    dualsense_.Set(DeviceEffect(device::kController),
                   cfg_.auraCorrection.brightness * Level(device::kController),
                   prefs_.dualsenseController && output_.Controls(device::kController) && !DeviceNative(prefs_, device::kController),
                   !IsHandoff(device::kController) && prefs_.dualsensePlayerLeds, prefs_.dualsensePlayers);
    hardware_.SetRamSticksApart(prefs_.setupRamApart);
    hardware_.SetRam(DeviceEffect(device::kRam), cfg_.auraCorrection.brightness * Level(device::kRam), prefs_.ramLighting,
                     output_.Controls(device::kRam) && !DeviceNative(prefs_, device::kRam), handoffActive_ || prefs_.handoffColorEnabled ? 2 : prefs_.ramRelease);
    // Armoury Crate's lighting service re-applies its own lighting whenever a USB device comes
    // or goes (dark, flickering fans and a stalled RAM effect): kept paused while LumaBridge has
    // ASUS lights. Opening its window must not silently transfer lighting ownership.
    const bool asusLights = !output_.stopped && (mirror_.IsRunning() || (prefs_.ramLighting && !DeviceNative(prefs_, device::kRam)));
    hardware_.SetPauseAsusLighting(prefs_.pauseArmouryCrate && asusLights);
}

bool Controller::IsHandoff(const std::string& id) const {
    return handoffActive_ && (handoffTargets_.empty() ||
        std::find(handoffTargets_.begin(), handoffTargets_.end(), id) != handoffTargets_.end() ||
        std::find(handoffTargets_.begin(), handoffTargets_.end(), ZoneDevice(id)) != handoffTargets_.end());
}

void Controller::SetDeviceNative(const std::string& id, bool native) {
    std::vector<std::string> targets{id};
    if (id == device::kFans || id == device::kBoard) targets = {device::kFans, device::kBoard};
    if (native && !DeviceNative(prefs_, id) && output_.Controls(id) && prefs_.handoffColorEnabled)
        PaintHandoffColor(2500, targets);
    for (const auto& target : targets) prefs_.deviceLighting[target].native = native;
    Changed();
}

void Controller::PaintHandoffColor(unsigned milliseconds, const std::vector<std::string>& targets) {
    handoffActive_ = true;
    handoffTargets_ = targets;
    handoffToken_ = std::max<uint64_t>(handoffToken_ + 1, GetTickCount64());
    const Output savedOutput = output_;
    output_.stopped = false;
    output_.game = false;
    output_.fanTest = false;
    // Prevent restoration from replacing the final frame on outputs that retain direct colors.
    if (IsHandoff(device::kOther)) {
        lampArray_.KeepLastColor(true);
        openRgb_.KeepLastColor(true);
    }
    const bool auraExpected = mirror_.GetStatus().connected;
    if (IsHandoff(device::kFans) || IsHandoff(device::kBoard)) {
        mirror_.Stop();
        Apply(output_);
    }
    const uint64_t started = GetTickCount64();
    const bool bluetoothWait = IsHandoff(device::kController) && prefs_.dualsenseController &&
        (handoffRecovery_ || dualsense_.state() == DualSenseOutput::State::Connecting);
    const uint64_t minimum = handoffRecovery_ ? 6000 : 200;
    const uint64_t budget = bluetoothWait ? 8000 : std::max<unsigned>(milliseconds, 2500);
    std::vector<std::string> pending;
    do {
        if (recoveryCancel_ && WaitForSingleObject(recoveryCancel_, 0) == WAIT_OBJECT_0) break;
        SendOutputs();
        pending.clear();
        auto expect = [&](const char* id, bool present, uint64_t delivered) {
            if (IsHandoff(id) && output_.Controls(id) && !DeviceNative(prefs_, id) && present && delivered < handoffToken_)
                pending.emplace_back(id);
        };
        const auto nollieDevices = nollie_.devices();
        const auto lampDevices = lampArray_.devices();
        const auto rgbDevices = openRgb_.devices();
        expect(device::kFans, auraExpected || mirror_.GetStatus().connected, mirror_.delivered());
        expect(device::kBoard, auraExpected || mirror_.GetStatus().connected, mirror_.delivered());
        expect(device::kMouse, prefs_.logitechDevices && logitech_.state() == LogitechOutput::State::Active, logitech_.delivered());
        expect(device::kKeyboard, prefs_.azothKeyboard && azoth_.state() == AzothOutput::State::Active, azoth_.delivered());
        expect(device::kController, prefs_.dualsenseController &&
               (dualsense_.state() == DualSenseOutput::State::Active || dualsense_.state() == DualSenseOutput::State::Connecting), dualsense_.delivered());
        expect(device::kRam, prefs_.ramLighting && hardware_.sticks() > 0, hardware_.delivered());
        expect(!targets.empty() && targets.front().rfind("nollie.", 0) == 0 ? targets.front().c_str() : device::kNollie, prefs_.nollieLighting &&
               std::any_of(nollieDevices.begin(), nollieDevices.end(), [&](const NollieDevice& hub) { return NollieOn(hub); }) &&
               std::any_of(prefs_.nollieChannels.begin(), prefs_.nollieChannels.end(), [](const auto& item) { return item.second.kind != nollie::Kind::Unused; }), nollie_.delivered());
        expect(device::kOther, prefs_.lampArray && std::any_of(lampDevices.begin(), lampDevices.end(), [&](const auto& d) { return LampArrayOn(d); }), lampArray_.delivered());
        expect(device::kOther, prefs_.openRgb && std::any_of(rgbDevices.begin(), rgbDevices.end(), [&](const auto& d) { return OpenRgbOn(d); }), openRgb_.delivered());
        expect(device::kOther, prefs_.nzxtLighting && kraken_.lightingActive(), kraken_.delivered());
        if (pending.empty() && GetTickCount64() - started >= minimum) break;
        Sleep(20);
    } while (GetTickCount64() - started < budget);
    for (const auto& id : pending) LUMA_WARN("handoff: no confirmed final frame for %s within %llu ms", id.c_str(),
                                            static_cast<unsigned long long>(budget));
    if (pending.empty()) LUMA_INFO("handoff: delivery checks complete for connected outputs");
    output_ = savedOutput;
    handoffActive_ = false;
    handoffTargets_.clear();
}

void Controller::RecoverHandoffColor(HANDLE cancel) {
    recoveryCancel_ = cancel;
    iniPath_ = UserConfigPath();
    cfg_ = LoadConfig(L"");
    prefs_ = LoadPrefs(iniPath_);
    if (!prefs_.handoffColorEnabled || prefs_.lightingStopped) return;
    handoffRecovery_ = true;
    cfg_.releaseControlOnShutdown = false;
    if (!cfg_.logFile.empty()) cfg_.logFile += L"-handoff.log";
    log::Init(cfg_.logFile, cfg_.logLevel);
    LUMA_INFO("Applying handoff color after unexpected exit");
    if (prefs_.logitechDevices) logitech_.Start(AppDirectory() + L"\\integrations\\LumaBridge_x64.dll");
    azoth_.Start();
    if (prefs_.dualsenseController) dualsense_.Start();
    openRgb_.Start(static_cast<uint16_t>(prefs_.openRgbPort));
    lampArray_.Start();
    nollie_.Start();
    hardware_.Start();
    // Kraken control is opt-in, and recovery probes only its documented supported models.
    RescanPresence();
    const catalog::AioModel* aio = presence_.Aio();
    if (prefs_.nzxtLighting && aio && aio->vid == nzxt::kVid) kraken_.Start(aio->pid, aio->lcd);
    PaintHandoffColor(5000);
    Shutdown(false);
}
}  // namespace luma::app
