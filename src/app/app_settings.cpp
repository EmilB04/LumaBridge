#include "app_settings.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace luma::app {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"LumaBridge";

std::wstring Read(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t buf[1024];
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
    v = Read(ini, L"App", L"WhenIdle");
    if (_wcsicmp(v.c_str(), L"armourycrate") == 0) p.idle = IdleBehavior::ArmouryCrate;
    if (_wcsicmp(v.c_str(), L"rainbow") == 0) p.idle = IdleBehavior::Rainbow;
    if (_wcsicmp(v.c_str(), L"off") == 0) p.idle = IdleBehavior::Off;
    v = Read(ini, L"App", L"LightingStopped");
    p.lightingStopped = v == L"1";
    v = Read(ini, L"App", L"StartMinimized");
    if (!v.empty()) p.startMinimized = v != L"0";

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
    return p;
}

void SaveAll(const std::wstring& ini, const Prefs& p, const Config& cfg) {
    WriteConfigValue(ini, L"App", L"Mode", p.mode == Mode::Manual ? L"manual" : L"auto");
    WriteConfigValue(ini, L"App", L"ManualColor", Widen(ToHex(p.manualColor)));
    WriteConfigValue(ini, L"App", L"ManualColor2", Widen(ToHex(p.manualColor2)));
    for (const auto& e : kEffectNames)
        if (e.kind == p.effect) WriteConfigValue(ini, L"App", L"ManualEffect", e.name);
    WriteConfigValue(ini, L"App", L"ManualSpeed", Num(p.speedHz));
    WriteConfigValue(ini, L"App", L"WhenIdle",
                     p.idle == IdleBehavior::Rainbow        ? L"rainbow"
                     : p.idle == IdleBehavior::Off          ? L"off"
                     : p.idle == IdleBehavior::ArmouryCrate ? L"armourycrate"
                                                            : L"manual");
    WriteConfigValue(ini, L"App", L"LightingStopped", p.lightingStopped ? L"1" : L"0");
    WriteConfigValue(ini, L"App", L"StartMinimized", p.startMinimized ? L"1" : L"0");
    std::string recent;
    for (size_t i = 0; i < p.recentColors.size(); ++i) recent += (i ? "," : "") + ToHex(p.recentColors[i]);
    WriteConfigValue(ini, L"App", L"RecentColors", Widen(recent));
    std::string games;
    for (size_t i = 0; i < p.lightingGames.size(); ++i) games += (i ? "|" : "") + p.lightingGames[i];
    WriteConfigValue(ini, L"App", L"LightingGames", Widen(games));

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
