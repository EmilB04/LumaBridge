#include "app_settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>

#include "pc_layout.h"
#include "display_layout.h"

namespace luma::app {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"LumaBridge";

std::wstring Read(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") {
    wchar_t buf[16384];  // ManualGames can hold several full paths
    GetPrivateProfileStringW(section, key, fallback, buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
    return buf;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c < 128 ? c : '?');
    return s;
}

// Ini names; "rainbow" (all LEDs cycling together) predates the per-LED effects.
constexpr struct {
    ManualEffect kind;
    const wchar_t* name;
} kEffectNames[] = {
    {ManualEffect::Static, L"static"},         {ManualEffect::Breathing, L"breathing"},
    {ManualEffect::Strobe, L"strobe"},         {ManualEffect::ColorCycle, L"rainbow"},
    {ManualEffect::RainbowWave, L"rainbowwave"}, {ManualEffect::Gradient, L"gradient"},
    {ManualEffect::Comet, L"comet"},           {ManualEffect::Twinkle, L"twinkle"},
};

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::wstring Num(double v) {
    wchar_t buf[32];
    swprintf(buf, std::size(buf), L"%g", v);
    return buf;
}

}  // namespace

std::string ToHex(Rgb c) {
    char buf[8];
    snprintf(buf, sizeof buf, "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

bool FromHex(const std::string& in, Rgb* out) {
    std::string s = in;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() != 6) return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(s.c_str(), &end, 16);
    if (*end != '\0') return false;
    *out = Rgb{static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    return true;
}

Prefs LoadPrefs(const std::wstring& ini) {
    Prefs p;
    std::wstring v = Read(ini, L"App", L"Mode");
    if (_wcsicmp(v.c_str(), L"manual") == 0) p.mode = Mode::Manual;
    FromHex(Narrow(Read(ini, L"App", L"ManualColor")), &p.manualColor);
    FromHex(Narrow(Read(ini, L"App", L"ManualColor2")), &p.manualColor2);
    FromHex(Narrow(Read(ini, L"App", L"PowerOnColor")), &p.powerOnColor);
    v = Read(ini, L"App", L"ManualEffect");
    for (const auto& e : kEffectNames)
        if (_wcsicmp(v.c_str(), e.name) == 0) p.effect = e.kind;
    v = Read(ini, L"App", L"ManualSpeed");
    if (!v.empty()) p.speedHz = static_cast<float>(settings::Number(v, 0.5));
    if (p.speedHz < 0.f || p.speedHz > 10.f) p.speedHz = 0.5f;
    auto num = [&](const wchar_t* key, float* out, float lo, float hi) {
        const std::wstring x = Read(ini, L"App", key);
        if (x.empty()) return;
        const float f = static_cast<float>(settings::Number(x, *out));
        if (f >= lo && f <= hi) *out = f;
    };
    num(L"RainbowHueStart", &p.rainbowHueStart, 0, 360);
    num(L"RainbowHueSpan", &p.rainbowHueSpan, 10, 360);
    num(L"RainbowSaturation", &p.rainbowSaturation, 0, 1);
    float spread = static_cast<float>(p.rainbowSpread);
    num(L"RainbowSpread", &spread, 1, 4);
    p.rainbowSpread = static_cast<int>(spread);
    p.effectReverse = Read(ini, L"App", L"EffectReverse") == L"1";
    v = Read(ini, L"App", L"WhenIdle");
    if (_wcsicmp(v.c_str(), L"armourycrate") == 0) p.idle = IdleBehavior::ArmouryCrate;
    if (_wcsicmp(v.c_str(), L"rainbow") == 0) p.idle = IdleBehavior::Rainbow;
    if (_wcsicmp(v.c_str(), L"off") == 0) p.idle = IdleBehavior::Off;
    v = Read(ini, L"App", L"LightingStopped");
    p.lightingStopped = v == L"1";
    v = Read(ini, L"App", L"StartMinimized");
    if (!v.empty()) p.startMinimized = v != L"0";
    v = Read(ini, L"App", L"CloseToTray");
    if (!v.empty()) p.closeToTray = v != L"0";
    v = Read(ini, L"App", L"Lighting3d");
    if (!v.empty()) p.lighting3d = v != L"0";
    float flow = static_cast<float>(p.flowDirection);
    num(L"FlowDirection", &flow, 0, kFlowDirections - 1);
    p.flowDirection = static_cast<int>(flow);
    num(L"FlowSeconds", &p.flowSeconds, 0.2f, 5.f);
    {
        // Setup effects ([Effects]).
        v = Read(ini, L"Effects", L"Effect");
        const int e = v.empty() ? 0 : settings::Integer(v);
        p.setupEffect = e >= fx::kFirstSetupEffect && e < fx::kFirstSetupEffect + fx::kSetupEffects ? e : 0;
        FromHex(Narrow(Read(ini, L"Effects", L"Color1")), &p.setupColor1);
        FromHex(Narrow(Read(ini, L"Effects", L"Color2")), &p.setupColor2);
        v = Read(ini, L"Effects", L"Speed");
        if (!v.empty()) p.setupSpeed = std::clamp(static_cast<float>(settings::Number(v, 0.5)), 0.1f, 10.f);
        v = Read(ini, L"Effects", L"Order");
        if (!v.empty()) p.setupOrder = std::clamp(settings::Integer(v), 1, kFlowDirections - 1);
        v = Read(ini, L"Effects", L"RamSticksApart");
        if (!v.empty()) p.setupRamApart = v != L"0";
    }
    v = Read(ini, L"App", L"DashGraphs");
    if (!v.empty()) p.dashGraphs = v != L"0";
    v = Read(ini, L"App", L"PsuWatts");
    if (!v.empty()) p.psuWatts = std::clamp(settings::Integer(v), 0, 5000);

    std::string recent = Narrow(Read(ini, L"App", L"RecentColors"));
    for (size_t pos = 0; pos < recent.size() && p.recentColors.size() < Prefs::kMaxRecent;) {
        size_t comma = recent.find(',', pos);
        Rgb c;
        if (FromHex(recent.substr(pos, comma - pos), &c)) p.recentColors.push_back(c);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    std::string games = Narrow(Read(ini, L"App", L"LightingGames"));
    for (size_t pos = 0; pos < games.size();) {
        size_t bar = games.find('|', pos);
        std::string g = games.substr(pos, bar - pos);
        if (!g.empty()) p.lightingGames.push_back(g);
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    p.screenForUnsupported = Read(ini, L"App", L"ScreenColorsForGames") == L"1";
    const std::string modes = Narrow(Read(ini, L"App", L"GameModes"));
    for (size_t pos = 0; pos < modes.size();) {
        size_t bar = modes.find('|', pos);
        const std::string item = modes.substr(pos, bar - pos);
        const size_t eq = item.find('=');
        if (eq != std::string::npos) {
            const std::string v = item.substr(eq + 1);
            if (v == "screen") p.gameModes[item.substr(0, eq)] = GameMode::Screen;
            if (v == "idle") p.gameModes[item.substr(0, eq)] = GameMode::Idle;
            if (v == "color") p.gameModes[item.substr(0, eq)] = GameMode::Color;
        }
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    const std::string colors = Narrow(Read(ini, L"App", L"GameColors"));
    for (size_t pos = 0; pos < colors.size();) {
        size_t bar = colors.find('|', pos);
        const std::string item = colors.substr(pos, bar - pos);
        const size_t eq = item.find('=');
        Rgb c;
        if (eq != std::string::npos && FromHex(item.substr(eq + 1), &c)) p.gameColors[item.substr(0, eq)] = c;
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    const std::string dash = Narrow(Read(ini, L"App", L"Dashboard"));
    if (!dash.empty()) {
        p.dashboard.clear();
        for (size_t pos = 0; pos <= dash.size();) {
            size_t comma = dash.find(',', pos);
            if (comma == std::string::npos) comma = dash.size();
            const std::string id = dash.substr(pos, comma - pos);
            // "lighting" / "game": now the fixed top row.
            if (!id.empty() && id != "-" && id != "lighting" && id != "game") p.dashboard.push_back(id);
            pos = comma + 1;
        }
        // Cards added since this list was saved start out shown.
        const std::string known = Narrow(Read(ini, L"App", L"DashboardKnown"));
        for (const auto& id : DefaultDashboard()) {
            const bool old = known.empty() ? id != "power" && id != "storage"
                                           : ("," + known + ",").find("," + id + ",") != std::string::npos;
            if (!old && std::find(p.dashboard.begin(), p.dashboard.end(), id) == p.dashboard.end())
                p.dashboard.push_back(id);
        }
    }
    v = Read(ini, L"Sleep", L"Logitech");
    if (!v.empty()) p.logitechSleep = v != L"0";
    v = Read(ini, L"Sleep", L"LogitechSeconds");
    if (!v.empty() && settings::Integer(v) > 0) p.logitechSleepSec = settings::Integer(v);
    v = Read(ini, L"Sleep", L"LogitechIgnoreDynamic");
    if (!v.empty()) p.logitechSleepIgnoreDynamic = v != L"0";
    v = Read(ini, L"Sleep", L"Azoth");
    if (!v.empty()) p.azothSleep = v != L"0";
    v = Read(ini, L"Sleep", L"AzothSeconds");
    if (!v.empty() && settings::Integer(v) > 0) p.azothSleepSec = settings::Integer(v);
    v = Read(ini, L"Sleep", L"AzothIgnoreDynamic");
    if (!v.empty()) p.azothSleepIgnoreDynamic = v != L"0";
    v = Read(ini, L"App", L"AzothKeyboard");
    if (!v.empty()) p.azothKeyboard = v != L"0";
    v = Read(ini, L"AzothOLED", L"Enabled");
    if (!v.empty()) p.azothOled.enabled = v != L"0";
    v = Read(ini, L"AzothOLED", L"Brightness");
    if (!v.empty()) p.azothOled.brightness = settings::Integer(v);
    v = Read(ini, L"AzothOLED", L"Content");
    if (!v.empty()) p.azothOled.content = static_cast<azoth::OledContent>(settings::Integer(v));
    v = Read(ini, L"AzothOLED", L"Animation");
    if (!v.empty()) p.azothOled.animation = settings::Integer(v);
    v = Read(ini, L"AzothOLED", L"AnimationSource");
    if (!v.empty()) p.azothOled.animationSource = static_cast<azoth::OledAnimationSource>(settings::Integer(v));
    v = Read(ini, L"AzothOLED", L"LumaAnimation");
    if (!v.empty()) p.azothOled.lumaAnimation = settings::Integer(v);
    v = Read(ini, L"AzothOLED", L"Clock12Hour");
    if (!v.empty()) p.azothOled.clock12Hour = v == L"1";
    v = Read(ini, L"AzothOLED", L"Cs2BombTimer");
    if (!v.empty()) p.azothOled.cs2BombTimer = v != L"0";
    v = Read(ini, L"AzothOLED", L"Cs2BombSeconds");
    if (!v.empty()) p.azothOled.cs2BombSeconds = settings::Integer(v);
    p.azothOled = azoth::NormalizeOled(p.azothOled);
    p.azothOledBanner = Read(ini, L"AzothOLED", L"Banner", p.azothOledBanner.c_str());
    v = Read(ini, L"AzothOLED", L"BannerSize");
    if (!v.empty()) p.azothOledBannerSize = std::clamp(settings::Integer(v), 8, 48);
    v = Read(ini, L"AzothOLED", L"BannerInvert");
    if (!v.empty()) p.azothOledBannerInvert = v == L"1";
    v = Read(ini, L"App", L"DualsenseController");
    if (!v.empty()) p.dualsenseController = v != L"0";
    v = Read(ini, L"App", L"DualsensePlayerLeds");
    if (!v.empty()) p.dualsensePlayerLeds = v != L"0";
    v = Read(ini, L"App", L"HandoffColorEnabled");
    if (!v.empty()) p.handoffColorEnabled = v == L"1";
    v = Read(ini, L"App", L"HandoffColor");
    if (!v.empty()) {
        const std::string hex(v.begin(), v.end());
        nollie::hubdetail::FromHex(hex, &p.handoffColor);
    }
    v = Read(ini, L"Controller", L"PlayerMode");
    if (!v.empty()) p.dualsensePlayers.mode = std::clamp(settings::Integer(v), 0, 4);
    v = Read(ini, L"Controller", L"PlayerMask");
    if (!v.empty()) p.dualsensePlayers.mask = static_cast<uint8_t>(settings::Integer(v) & 31);
    v = Read(ini, L"Controller", L"PlayerEffects");
    if (!v.empty()) p.dualsensePlayers.effects = static_cast<uint32_t>(std::wcstoul(v.c_str(), nullptr, 10));
    v = Read(ini, L"Controller", L"PlayerSpeed");
    if (!v.empty()) {
        const double speed = settings::Number(v, 0.5);
        p.dualsensePlayers.speed = std::isfinite(speed) ? std::clamp(speed, 0.1, 3.0) : 0.5;
    }
    v = Read(ini, L"Controller", L"Input");
    if (!v.empty()) p.padInput = v != L"0";
    v = Read(ini, L"Controller", L"ShowValues");
    if (!v.empty()) p.padShowValues = v != L"0";
    v = Read(ini, L"Controller", L"View3d");
    if (!v.empty()) p.padView3d = v != L"0";
    v = Read(ini, L"Controller", L"ShowTiming");
    if (!v.empty()) p.padShowTiming = v != L"0";
    v = Read(ini, L"Controller", L"MapEnabled");
    if (!v.empty()) p.padMapping.enabled = v != L"0";
    v = Read(ini, L"Controller", L"MapMouseStick");
    if (!v.empty()) p.padMapping.rightStickMouse = v != L"0";
    v = Read(ini, L"Controller", L"MouseSpeed");
    if (!v.empty()) p.padMapping.mouseSpeed = std::clamp(settings::Integer(v), 1, 30);
    v = Read(ini, L"Controller", L"Deadzone");
    if (!v.empty()) p.padMapping.deadzone = std::clamp(settings::Integer(v), 0, 50);
    for (int b = 0; b < pad::kButtonCount; ++b)
        p.padMapping.buttons[static_cast<size_t>(b)] =
            pad::DecodeBinding(Narrow(Read(ini, L"Controller", (L"Map." + Widen(pad::ButtonName(b))).c_str())));
    v = Read(ini, L"App", L"RamLighting");
    if (!v.empty()) p.ramLighting = v != L"0";
    v = Read(ini, L"App", L"PauseArmouryCrate");
    if (!v.empty()) p.pauseArmouryCrate = v != L"0";
    v = Read(ini, L"App", L"NzxtLighting");
    if (!v.empty()) p.nzxtLighting = v != L"0";
    p.setupDone = Read(ini, L"App", L"SetupDone") == L"1";
    v = Read(ini, L"LampArray", L"Enabled");
    if (!v.empty()) p.lampArray = v != L"0";
    {
        const std::string list = Narrow(Read(ini, L"LampArray", L"Devices"));  // "name=1|name=0"
        for (size_t pos = 0; pos < list.size();) {
            size_t bar = list.find('|', pos);
            const std::string item = list.substr(pos, bar - pos);
            const size_t eq = item.rfind('=');
            if (eq != std::string::npos && eq > 0) p.lampArrayDevices[item.substr(0, eq)] = item.substr(eq + 1) == "1";
            if (bar == std::string::npos) break;
            pos = bar + 1;
        }
    }
    v = Read(ini, L"OpenRGB", L"Enabled");
    if (!v.empty()) p.openRgb = v != L"0";
    v = Read(ini, L"OpenRGB", L"Port");
    if (!v.empty() && settings::Integer(v) > 0 && settings::Integer(v) < 65536) p.openRgbPort = settings::Integer(v);
    {
        const std::string list = Narrow(Read(ini, L"OpenRGB", L"Devices"));  // "name=1|name=0"
        for (size_t pos = 0; pos < list.size();) {
            size_t bar = list.find('|', pos);
            const std::string item = list.substr(pos, bar - pos);
            const size_t eq = item.rfind('=');
            if (eq != std::string::npos && eq > 0) p.openRgbDevices[item.substr(0, eq)] = item.substr(eq + 1) == "1";
            if (bar == std::string::npos) break;
            pos = bar + 1;
        }
    }
    v = Read(ini, L"Nollie", L"Enabled");
    if (!v.empty()) p.nollieLighting = v != L"0";
    {
        const std::string list = Narrow(Read(ini, L"Nollie", L"Devices"));  // "id=1|id=0"
        for (size_t pos = 0; pos < list.size();) {
            size_t bar = list.find('|', pos);
            const std::string item = list.substr(pos, bar - pos);
            const size_t eq = item.rfind('=');
            if (eq != std::string::npos && eq > 0) p.nollieDevices[item.substr(0, eq)] = item.substr(eq + 1) == "1";
            if (bar == std::string::npos) break;
            pos = bar + 1;
        }
        const std::string bootIds = Narrow(Read(ini, L"Nollie", L"BootIds"));  // "id,id"
        for (size_t pos = 0; pos < bootIds.size();) {
            size_t comma = bootIds.find(',', pos);
            const std::string id = bootIds.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            nollie::onboard::BootPlan b;
            if (!id.empty() && nollie::onboard::Decode(Narrow(Read(ini, L"Nollie", (L"Boot." + Widen(id)).c_str())), &b))
                p.nollieOnboardBoot[id] = b;
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        const std::string hubs = Narrow(Read(ini, L"Nollie", L"Hubs"));  // "id,id"
        for (size_t pos = 0; pos < hubs.size();) {
            size_t comma = hubs.find(',', pos);
            const std::string id = hubs.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            nollie::Hub h;
            if (!id.empty() && nollie::Decode(Narrow(Read(ini, L"Nollie", (L"Hub." + Widen(id)).c_str())), &h)) p.nollieHubs[id] = h;
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        const std::string keys = Narrow(Read(ini, L"Nollie", L"Channels"));  // "id.0,id.5"
        for (size_t pos = 0; pos < keys.size();) {
            size_t comma = keys.find(',', pos);
            const std::string key = keys.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            nollie::Channel c;
            if (!key.empty() && nollie::Decode(Narrow(Read(ini, L"Nollie", (L"Channel." + Widen(key)).c_str())), &c)) {
                p.nollieChannels[key] = c;
                DeviceLighting d;
                if (DecodeDevice(Narrow(Read(ini, L"Nollie", (L"Look." + Widen(key)).c_str())), &d))
                    p.deviceLighting["nollie." + key] = d;
                for (int fan = 0; c.kind == nollie::Kind::Fans && fan < c.Fans(); ++fan) {  // each fan's own
                    DeviceLighting fl;
                    const std::string fanKey = key + ".f" + std::to_string(fan);
                    if (DecodeDevice(Narrow(Read(ini, L"Nollie", (L"Look." + Widen(fanKey)).c_str())), &fl))
                        p.deviceLighting["nollie." + fanKey] = fl;
                }
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    {
        // Lighting zones ([Zones]).
        v = Read(ini, L"Zones", L"Count");
        const int count = v.empty() ? 0 : std::clamp(settings::Integer(v), 0, static_cast<int>(kMaxZones));
        for (int i = 0; i < count; ++i) {
            Zone z;
            if (Decode(Narrow(Read(ini, L"Zones", (L"Zone" + std::to_wstring(i)).c_str())), &z)) p.zones.push_back(z);
        }
        Normalize(p.zones);
        p.zonesMigrated = Read(ini, L"Zones", L"Migrated") == L"1";
    }
    p.fansAsked = Read(ini, L"App", L"FansAsked") == L"1";
    {
        const std::string asked = Narrow(Read(ini, L"Nollie", L"Asked"));  // "id,id"
        for (size_t pos = 0; pos < asked.size();) {
            const size_t comma = asked.find(',', pos);
            const std::string id = asked.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!id.empty()) p.nollieAsked.push_back(id);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    v = Read(ini, L"App", L"RamSlots");
    if (!v.empty()) p.ramSlots = v == L"auto" ? -1 : (settings::Integer(v) & 15);
    v = Read(ini, L"App", L"RamRelease");
    if (v == L"off") p.ramRelease = 1;
    if (v == L"keep") p.ramRelease = 2;
    v = Read(ini, L"App", L"LogitechDevices");
    if (!v.empty()) p.logitechDevices = v != L"0";
    v = Read(ini, L"App", L"LogitechKeepInGames");  // "LogitechForce" / "LogitechKeep" before 0.10.1
    if (!v.empty()) p.logitechForce = v != L"0";
    v = Read(ini, L"Games", L"ForzaPort");
    if (!v.empty() && settings::Integer(v) > 0 && settings::Integer(v) < 65536) p.forzaPort = settings::Integer(v);
    for (auto [key, port] : {std::pair<const wchar_t*, int*>{L"BeamngPort", &p.beamngPort}, {L"DirtPort", &p.dirtPort},
                             {L"Ams2Port", &p.ams2Port}, {L"XplanePort", &p.xplanePort}, {L"WrcPort", &p.wrcPort}}) {
        v = Read(ini, L"Games", key);
        if (!v.empty() && settings::Integer(v) > 0 && settings::Integer(v) < 65536) *port = settings::Integer(v);
    }
    v = Read(ini, L"Games", L"F1Port");
    if (!v.empty() && settings::Integer(v) > 0 && settings::Integer(v) < 65536) p.f1Port = settings::Integer(v);
    v = Read(ini, L"Dashboard", L"LhmPort");
    if (!v.empty()) p.lhmPort = settings::Integer(v);
    for (const char* id : device::All()) {
        DeviceLighting d;
        if (DecodeDevice(Narrow(Read(ini, L"Lighting", (L"Device." + Widen(id)).c_str())), &d)) p.deviceLighting[id] = d;
    }
    if (!p.zonesMigrated) {
        // Before zones a device could have a look of its own; that is a zone of one now.
        for (const Zone& z : ZonesFromOwnLooks(p.deviceLighting, p.zones)) p.zones.push_back(z);
        p.zonesMigrated = true;
    }
    std::vector<std::string> items{device::kBoard, device::kRam, device::kMouse, device::kKeyboard};
    for (int i = 0; i < 8; ++i) items.push_back(FanItem(i));
    for (const char* item : {"gpu", "headset", "controller"}) items.push_back(item);
    for (int i = 0; i < pc::kMaxOthers; ++i) items.push_back("other" + std::to_string(i));
    for (const char* item : pc::DeskItems()) items.push_back(item);
    for (const std::string& item : items) {
        Spot s;
        if (DecodeSpot(Narrow(Read(ini, L"Lighting", (L"Spot." + Widen(item)).c_str())), &s)) p.setupSpots[item] = s;
    }
    p.caseLayout = Narrow(Read(ini, L"Lighting", L"CaseLayout"));
    p.monitorSizes = displays::DecodeSizes(Narrow(Read(ini, L"Displays", L"Sizes")));
    const std::wstring manual = Read(ini, L"App", L"ManualGames");
    for (size_t pos = 0; pos < manual.size();) {
        size_t bar = manual.find(L'|', pos);
        std::wstring g = manual.substr(pos, bar - pos);
        if (!g.empty()) p.manualGames.push_back(g);
        if (bar == std::wstring::npos) break;
        pos = bar + 1;
    }
    return p;
}

void SaveAll(const std::wstring& ini, const Prefs& p, const Config& cfg) {
    WriteConfigValue(ini, L"App", L"Mode", p.mode == Mode::Manual ? L"manual" : L"auto");
    WriteConfigValue(ini, L"App", L"ManualColor", Widen(ToHex(p.manualColor)));
    WriteConfigValue(ini, L"App", L"ManualColor2", Widen(ToHex(p.manualColor2)));
    WriteConfigValue(ini, L"App", L"PowerOnColor", Widen(ToHex(p.powerOnColor)));
    for (const auto& e : kEffectNames)
        if (e.kind == p.effect) WriteConfigValue(ini, L"App", L"ManualEffect", e.name);
    WriteConfigValue(ini, L"App", L"ManualSpeed", Num(p.speedHz));
    WriteConfigValue(ini, L"App", L"RainbowHueStart", Num(p.rainbowHueStart));
    WriteConfigValue(ini, L"App", L"RainbowHueSpan", Num(p.rainbowHueSpan));
    WriteConfigValue(ini, L"App", L"RainbowSaturation", Num(p.rainbowSaturation));
    WriteConfigValue(ini, L"App", L"RainbowSpread", Num(p.rainbowSpread));
    WriteConfigValue(ini, L"App", L"EffectReverse", p.effectReverse ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"WhenIdle",
                     p.idle == IdleBehavior::Rainbow        ? L"rainbow"
                     : p.idle == IdleBehavior::Off          ? L"off"
                     : p.idle == IdleBehavior::ArmouryCrate ? L"armourycrate"
                                                            : L"manual");
    WriteConfigValue(ini, L"App", L"LightingStopped", p.lightingStopped ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"StartMinimized", p.startMinimized ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"CloseToTray", p.closeToTray ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"Lighting3d", p.lighting3d ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"FlowDirection", std::to_wstring(p.flowDirection));
    WriteConfigValue(ini, L"App", L"FlowSeconds", Num(p.flowSeconds));
    WriteConfigValue(ini, L"Effects", L"Effect", std::to_wstring(p.setupEffect));
    WriteConfigValue(ini, L"Effects", L"Color1", Widen(ToHex(p.setupColor1)));
    WriteConfigValue(ini, L"Effects", L"Color2", Widen(ToHex(p.setupColor2)));
    WriteConfigValue(ini, L"Effects", L"Speed", Num(p.setupSpeed));
    WriteConfigValue(ini, L"Effects", L"Order", std::to_wstring(p.setupOrder));
    WriteConfigValue(ini, L"Effects", L"RamSticksApart", p.setupRamApart ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"DashGraphs", p.dashGraphs ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"PsuWatts", std::to_wstring(p.psuWatts).c_str());
    WriteConfigValue(ini, L"App", L"SetupDone", p.setupDone ? L"1" : L"0");
    WriteConfigValue(ini, L"LampArray", L"Enabled", p.lampArray ? L"1" : L"0");
    {
        std::string list;
        for (const auto& [name, on] : p.lampArrayDevices) list += (list.empty() ? "" : "|") + name + (on ? "=1" : "=0");
        WriteConfigValue(ini, L"LampArray", L"Devices", Widen(list));
    }
    WriteConfigValue(ini, L"OpenRGB", L"Enabled", p.openRgb ? L"1" : L"0");
    WriteConfigValue(ini, L"OpenRGB", L"Port", Num(p.openRgbPort));
    {
        std::string list;
        for (const auto& [name, on] : p.openRgbDevices) list += (list.empty() ? "" : "|") + name + (on ? "=1" : "=0");
        WriteConfigValue(ini, L"OpenRGB", L"Devices", Widen(list));
    }
    WriteConfigValue(ini, L"App", L"FansAsked", p.fansAsked ? L"1" : L"0");
    {
        std::string asked;
        for (const std::string& id : p.nollieAsked) asked += (asked.empty() ? "" : ",") + id;
        WriteConfigValue(ini, L"Nollie", L"Asked", Widen(asked));
    }
    WriteConfigValue(ini, L"Zones", L"Migrated", p.zonesMigrated ? L"1" : L"0");
    WriteConfigValue(ini, L"Zones", L"Count", std::to_wstring(p.zones.size()));
    for (size_t i = 0; i < kMaxZones; ++i)
        WriteConfigValue(ini, L"Zones", (L"Zone" + std::to_wstring(i)).c_str(), i < p.zones.size() ? Widen(Encode(p.zones[i])) : L"");
    WriteConfigValue(ini, L"Nollie", L"Enabled", p.nollieLighting ? L"1" : L"0");
    {
        std::string list, keys;
        for (const auto& [id, on] : p.nollieDevices) list += (list.empty() ? "" : "|") + id + (on ? "=1" : "=0");
        WriteConfigValue(ini, L"Nollie", L"Devices", Widen(list));
        for (const auto& [key, c] : p.nollieChannels) {
            keys += (keys.empty() ? "" : ",") + key;
            WriteConfigValue(ini, L"Nollie", (L"Channel." + Widen(key)).c_str(), Widen(nollie::Encode(c)));
            auto look = p.deviceLighting.find("nollie." + key);
            if (look != p.deviceLighting.end())
                WriteConfigValue(ini, L"Nollie", (L"Look." + Widen(key)).c_str(), Widen(EncodeDevice(look->second)));
            for (int fan = 0; c.kind == nollie::Kind::Fans && fan < c.Fans(); ++fan) {
                const std::string fanKey = key + ".f" + std::to_string(fan);
                auto fl = p.deviceLighting.find("nollie." + fanKey);
                if (fl != p.deviceLighting.end())
                    WriteConfigValue(ini, L"Nollie", (L"Look." + Widen(fanKey)).c_str(), Widen(EncodeDevice(fl->second)));
            }
        }
        WriteConfigValue(ini, L"Nollie", L"Channels", Widen(keys));
        std::string hubs;
        for (const auto& [id, h] : p.nollieHubs) {
            hubs += (hubs.empty() ? "" : ",") + id;
            WriteConfigValue(ini, L"Nollie", (L"Hub." + Widen(id)).c_str(), Widen(nollie::Encode(h)));
        }
        WriteConfigValue(ini, L"Nollie", L"Hubs", Widen(hubs));
        std::string bootIds;
        for (const auto& [id, b] : p.nollieOnboardBoot) {
            bootIds += (bootIds.empty() ? "" : ",") + id;
            WriteConfigValue(ini, L"Nollie", (L"Boot." + Widen(id)).c_str(), Widen(nollie::onboard::Encode(b)));
        }
        WriteConfigValue(ini, L"Nollie", L"BootIds", Widen(bootIds));
    }
    std::string recent;
    for (size_t i = 0; i < p.recentColors.size(); ++i) recent += (i ? "," : "") + ToHex(p.recentColors[i]);
    WriteConfigValue(ini, L"App", L"RecentColors", Widen(recent));
    std::string games;
    for (size_t i = 0; i < p.lightingGames.size(); ++i) games += (i ? "|" : "") + p.lightingGames[i];
    WriteConfigValue(ini, L"App", L"LightingGames", Widen(games));
    std::wstring manual;
    for (size_t i = 0; i < p.manualGames.size(); ++i) manual += (i ? L"|" : L"") + p.manualGames[i];
    WriteConfigValue(ini, L"App", L"ManualGames", manual);
    WriteConfigValue(ini, L"App", L"ScreenColorsForGames", p.screenForUnsupported ? L"1" : L"0");
    std::string modes;
    for (const auto& [key, mode] : p.gameModes) {
        if (mode == GameMode::Default) continue;
        modes += (modes.empty() ? "" : "|") + key +
                 (mode == GameMode::Screen ? "=screen" : mode == GameMode::Color ? "=color" : "=idle");
    }
    WriteConfigValue(ini, L"App", L"GameModes", Widen(modes));
    std::string colors;
    for (const auto& [key, c] : p.gameColors) colors += (colors.empty() ? "" : "|") + key + "=" + ToHex(c);
    WriteConfigValue(ini, L"App", L"GameColors", Widen(colors));
    std::string dash;
    for (size_t i = 0; i < p.dashboard.size(); ++i) dash += (i ? "," : "") + p.dashboard[i];
    WriteConfigValue(ini, L"App", L"Dashboard", Widen(dash.empty() ? "-" : dash));  // "-": all hidden
    std::string known;
    for (const auto& id : DefaultDashboard()) known += (known.empty() ? "" : ",") + id;
    WriteConfigValue(ini, L"App", L"DashboardKnown", Widen(known));
    WriteConfigValue(ini, L"Dashboard", L"LhmPort", Num(p.lhmPort));
    WriteConfigValue(ini, L"Games", L"ForzaPort", Num(p.forzaPort));
    WriteConfigValue(ini, L"Games", L"F1Port", Num(p.f1Port));
    WriteConfigValue(ini, L"Games", L"BeamngPort", Num(p.beamngPort));
    WriteConfigValue(ini, L"Games", L"DirtPort", Num(p.dirtPort));
    WriteConfigValue(ini, L"Games", L"Ams2Port", Num(p.ams2Port));
    WriteConfigValue(ini, L"Games", L"XplanePort", Num(p.xplanePort));
    WriteConfigValue(ini, L"Games", L"WrcPort", Num(p.wrcPort));
    WriteConfigValue(ini, L"App", L"LogitechDevices", p.logitechDevices ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"LogitechKeepInGames", p.logitechForce ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"AzothKeyboard", p.azothKeyboard ? L"1" : L"0");
    WriteConfigValue(ini, L"AzothOLED", L"Enabled", p.azothOled.enabled ? L"1" : L"0");
    WriteConfigValue(ini, L"AzothOLED", L"Brightness", Num(p.azothOled.brightness));
    WriteConfigValue(ini, L"AzothOLED", L"Content", Num(static_cast<int>(p.azothOled.content)));
    WriteConfigValue(ini, L"AzothOLED", L"Animation", Num(p.azothOled.animation));
    WriteConfigValue(ini, L"AzothOLED", L"AnimationSource", Num(static_cast<int>(p.azothOled.animationSource)));
    WriteConfigValue(ini, L"AzothOLED", L"LumaAnimation", Num(p.azothOled.lumaAnimation));
    WriteConfigValue(ini, L"AzothOLED", L"Clock12Hour", p.azothOled.clock12Hour ? L"1" : L"0");
    WriteConfigValue(ini, L"AzothOLED", L"Cs2BombTimer", p.azothOled.cs2BombTimer ? L"1" : L"0");
    WriteConfigValue(ini, L"AzothOLED", L"Cs2BombSeconds", Num(p.azothOled.cs2BombSeconds));
    WriteConfigValue(ini, L"AzothOLED", L"Banner", p.azothOledBanner);
    WriteConfigValue(ini, L"AzothOLED", L"BannerSize", Num(p.azothOledBannerSize));
    WriteConfigValue(ini, L"AzothOLED", L"BannerInvert", p.azothOledBannerInvert ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"DualsenseController", p.dualsenseController ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"DualsensePlayerLeds", p.dualsensePlayerLeds ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"HandoffColorEnabled", p.handoffColorEnabled ? L"1" : L"0");
    const std::string handoffHex = nollie::hubdetail::Hex(p.handoffColor);
    WriteConfigValue(ini, L"App", L"HandoffColor", std::wstring(handoffHex.begin(), handoffHex.end()).c_str());
    WriteConfigValue(ini, L"Controller", L"PlayerMode", std::to_wstring(p.dualsensePlayers.mode).c_str());
    WriteConfigValue(ini, L"Controller", L"PlayerMask", std::to_wstring(p.dualsensePlayers.mask).c_str());
    WriteConfigValue(ini, L"Controller", L"PlayerEffects", std::to_wstring(p.dualsensePlayers.effects).c_str());
    WriteConfigValue(ini, L"Controller", L"PlayerSpeed", std::to_wstring(p.dualsensePlayers.speed).c_str());
    WriteConfigValue(ini, L"Controller", L"Input", p.padInput ? L"1" : L"0");
    WriteConfigValue(ini, L"Controller", L"ShowValues", p.padShowValues ? L"1" : L"0");
    WriteConfigValue(ini, L"Controller", L"View3d", p.padView3d ? L"1" : L"0");
    WriteConfigValue(ini, L"Controller", L"ShowTiming", p.padShowTiming ? L"1" : L"0");
    WriteConfigValue(ini, L"Controller", L"MapEnabled", p.padMapping.enabled ? L"1" : L"0");
    WriteConfigValue(ini, L"Controller", L"MapMouseStick", p.padMapping.rightStickMouse ? L"1" : L"0");
    WriteConfigValue(ini, L"Controller", L"MouseSpeed", Num(p.padMapping.mouseSpeed));
    WriteConfigValue(ini, L"Controller", L"Deadzone", Num(p.padMapping.deadzone));
    for (int b = 0; b < pad::kButtonCount; ++b)
        WriteConfigValue(ini, L"Controller", (L"Map." + Widen(pad::ButtonName(b))).c_str(),
                         Widen(pad::EncodeBinding(p.padMapping.buttons[static_cast<size_t>(b)])));
    WriteConfigValue(ini, L"Sleep", L"Logitech", p.logitechSleep ? L"1" : L"0");
    WriteConfigValue(ini, L"Sleep", L"LogitechSeconds", Num(p.logitechSleepSec));
    WriteConfigValue(ini, L"Sleep", L"LogitechIgnoreDynamic", p.logitechSleepIgnoreDynamic ? L"1" : L"0");
    WriteConfigValue(ini, L"Sleep", L"Azoth", p.azothSleep ? L"1" : L"0");
    WriteConfigValue(ini, L"Sleep", L"AzothSeconds", Num(p.azothSleepSec));
    WriteConfigValue(ini, L"Sleep", L"AzothIgnoreDynamic", p.azothSleepIgnoreDynamic ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"RamLighting", p.ramLighting ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"PauseArmouryCrate", p.pauseArmouryCrate ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"NzxtLighting", p.nzxtLighting ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"RamSlots", p.ramSlots < 0 ? L"auto" : Num(p.ramSlots));
    WriteConfigValue(ini, L"App", L"RamRelease", p.ramRelease == 1 ? L"off" : p.ramRelease == 2 ? L"keep" : L"rainbow");

    for (const auto& [id, d] : p.deviceLighting)
        WriteConfigValue(ini, L"Lighting", (L"Device." + Widen(id)).c_str(), Widen(EncodeDevice(d)));
    for (const auto& [item, spot] : p.setupSpots)
        WriteConfigValue(ini, L"Lighting", (L"Spot." + Widen(item)).c_str(), Widen(EncodeSpot(spot)));
    for (const char* item : pc::DeskItems())  // moved back to where LumaBridge puts it
        if (!p.setupSpots.count(item)) WriteConfigValue(ini, L"Lighting", (L"Spot." + Widen(item)).c_str(), L"");
    WriteConfigValue(ini, L"Lighting", L"CaseLayout", Widen(p.caseLayout));
    WriteConfigValue(ini, L"Displays", L"Sizes", Widen(displays::EncodeSizes(p.monitorSizes)));

    WriteConfigValue(ini, L"Color", L"Brightness", Num(cfg.auraCorrection.brightness * 100.0));
    WriteConfigValue(ini, L"Color", L"GainR", Num(cfg.auraCorrection.gainR * 100.0));
    WriteConfigValue(ini, L"Color", L"GainG", Num(cfg.auraCorrection.gainG * 100.0));
    WriteConfigValue(ini, L"Color", L"GainB", Num(cfg.auraCorrection.gainB * 100.0));
    WriteConfigValue(ini, L"Color", L"Gamma", Num(cfg.auraCorrection.gamma));
    WriteConfigValue(ini, L"Aura", L"MaxUpdateHz", Num(cfg.maxUpdateHz));
    std::wstring disabled;
    for (size_t i = 0; i < cfg.auraDisabledDevices.size(); ++i)
        disabled += (i ? L"|" : L"") + cfg.auraDisabledDevices[i];
    WriteConfigValue(ini, L"Aura", L"DisabledDevices", disabled);
    WriteConfigValue(ini, L"Aura", L"ArgbFans", Num(cfg.argbFans.Fans()));
    WriteConfigValue(ini, L"Aura", L"ArgbLedsPerFan", Num(cfg.argbFans.LedsPerFan()));
    WriteConfigValue(ini, L"Aura", L"ArgbFanLayout", cfg.argbFans.repeatPerFan ? L"repeat" : L"span");
    WriteConfigValue(ini, L"Mirror", L"BitmapMode",
                     cfg.bitmapReduce == BitmapReduce::Brightest ? L"brightest" : L"average");
    WriteConfigValue(ini, L"GameSense", L"Enabled", cfg.gameSenseEnabled ? L"1" : L"0");
}

bool IsAutostartEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    bool found = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

bool SetAutostart(bool enable) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS rc;
    if (enable) {
        wchar_t exe[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --minimized";
        rc = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kRunValue);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

}  // namespace luma::app
