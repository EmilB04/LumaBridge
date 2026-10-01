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

std::wstring Read(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t buf[16384];  // ManualGames can hold several full paths
    GetPrivateProfileStringW(section, key, L"", buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
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
    v = Read(ini, L"App", L"ManualEffect");
    for (const auto& e : kEffectNames)
        if (_wcsicmp(v.c_str(), e.name) == 0) p.effect = e.kind;
    v = Read(ini, L"App", L"ManualSpeed");
    if (!v.empty()) p.speedHz = static_cast<float>(_wtof(v.c_str()));
    if (p.speedHz < 0.f || p.speedHz > 10.f) p.speedHz = 0.5f;
    auto num = [&](const wchar_t* key, float* out, float lo, float hi) {
        const std::wstring x = Read(ini, L"App", key);
        if (x.empty()) return;
        const float f = static_cast<float>(_wtof(x.c_str()));
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
    v = Read(ini, L"App", L"DashGraphs");
    if (!v.empty()) p.dashGraphs = v != L"0";
    v = Read(ini, L"App", L"PsuWatts");
    if (!v.empty()) p.psuWatts = std::clamp(_wtoi(v.c_str()), 0, 5000);

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
    if (!v.empty() && _wtoi(v.c_str()) > 0) p.logitechSleepSec = _wtoi(v.c_str());
    v = Read(ini, L"Sleep", L"LogitechIgnoreDynamic");
    if (!v.empty()) p.logitechSleepIgnoreDynamic = v != L"0";
    v = Read(ini, L"Sleep", L"Azoth");
    if (!v.empty()) p.azothSleep = v != L"0";
    v = Read(ini, L"Sleep", L"AzothSeconds");
    if (!v.empty() && _wtoi(v.c_str()) > 0) p.azothSleepSec = _wtoi(v.c_str());
    v = Read(ini, L"Sleep", L"AzothIgnoreDynamic");
    if (!v.empty()) p.azothSleepIgnoreDynamic = v != L"0";
    v = Read(ini, L"App", L"AzothKeyboard");
    if (!v.empty()) p.azothKeyboard = v != L"0";
    v = Read(ini, L"App", L"DualsenseController");
    if (!v.empty()) p.dualsenseController = v != L"0";
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
    if (!v.empty()) p.padMapping.mouseSpeed = std::clamp(_wtoi(v.c_str()), 1, 30);
    v = Read(ini, L"Controller", L"Deadzone");
    if (!v.empty()) p.padMapping.deadzone = std::clamp(_wtoi(v.c_str()), 0, 50);
    for (int b = 0; b < pad::kButtonCount; ++b)
        p.padMapping.buttons[static_cast<size_t>(b)] =
            pad::DecodeBinding(Narrow(Read(ini, L"Controller", (L"Map." + Widen(pad::ButtonName(b))).c_str())));
    v = Read(ini, L"App", L"RamLighting");
    if (!v.empty()) p.ramLighting = v != L"0";
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
    if (!v.empty() && _wtoi(v.c_str()) > 0 && _wtoi(v.c_str()) < 65536) p.openRgbPort = _wtoi(v.c_str());
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
    v = Read(ini, L"App", L"RamSlots");
    if (!v.empty()) p.ramSlots = v == L"auto" ? -1 : (_wtoi(v.c_str()) & 15);
    v = Read(ini, L"App", L"RamRelease");
    if (v == L"off") p.ramRelease = 1;
    if (v == L"keep") p.ramRelease = 2;
    v = Read(ini, L"App", L"LogitechDevices");
    if (!v.empty()) p.logitechDevices = v != L"0";
    v = Read(ini, L"App", L"LogitechKeepInGames");  // "LogitechForce" / "LogitechKeep" before 0.10.1
    if (!v.empty()) p.logitechForce = v != L"0";
    v = Read(ini, L"Games", L"ForzaPort");
    if (!v.empty() && _wtoi(v.c_str()) > 0 && _wtoi(v.c_str()) < 65536) p.forzaPort = _wtoi(v.c_str());
    for (auto [key, port] : {std::pair<const wchar_t*, int*>{L"BeamngPort", &p.beamngPort}, {L"DirtPort", &p.dirtPort},
                             {L"Ams2Port", &p.ams2Port}, {L"XplanePort", &p.xplanePort}}) {
        v = Read(ini, L"Games", key);
        if (!v.empty() && _wtoi(v.c_str()) > 0 && _wtoi(v.c_str()) < 65536) *port = _wtoi(v.c_str());
    }
    v = Read(ini, L"Games", L"F1Port");
    if (!v.empty() && _wtoi(v.c_str()) > 0 && _wtoi(v.c_str()) < 65536) p.f1Port = _wtoi(v.c_str());
    v = Read(ini, L"Dashboard", L"LhmPort");
    if (!v.empty()) p.lhmPort = _wtoi(v.c_str());
    for (const char* id : device::All()) {
        DeviceLighting d;
        if (DecodeDevice(Narrow(Read(ini, L"Lighting", (L"Device." + Widen(id)).c_str())), &d)) p.deviceLighting[id] = d;
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
    WriteConfigValue(ini, L"App", L"LogitechDevices", p.logitechDevices ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"LogitechKeepInGames", p.logitechForce ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"AzothKeyboard", p.azothKeyboard ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"DualsenseController", p.dualsenseController ? L"1" : L"0");
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
