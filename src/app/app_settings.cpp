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
    v = Read(ini, L"App", L"ManualEffect");
    if (_wcsicmp(v.c_str(), L"breathing") == 0) p.effect = ManualEffect::Breathing;
    if (_wcsicmp(v.c_str(), L"strobe") == 0) p.effect = ManualEffect::Strobe;
    v = Read(ini, L"App", L"ManualSpeed");
    if (!v.empty()) p.speedHz = static_cast<float>(_wtof(v.c_str()));
    if (p.speedHz < 0.1f || p.speedHz > 10.f) p.speedHz = 0.5f;
    v = Read(ini, L"App", L"WhenIdle");
    if (_wcsicmp(v.c_str(), L"manual") == 0) p.idle = IdleBehavior::ManualColor;
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
    return p;
}

void SaveAll(const std::wstring& ini, const Prefs& p, const Config& cfg) {
    WriteConfigValue(ini, L"App", L"Mode", p.mode == Mode::Manual ? L"manual" : L"auto");
    WriteConfigValue(ini, L"App", L"ManualColor", Widen(ToHex(p.manualColor)));
    WriteConfigValue(ini, L"App", L"ManualEffect",
                     p.effect == ManualEffect::Breathing ? L"breathing"
                     : p.effect == ManualEffect::Strobe  ? L"strobe"
                                                         : L"static");
    WriteConfigValue(ini, L"App", L"ManualSpeed", Num(p.speedHz));
    WriteConfigValue(ini, L"App", L"WhenIdle", p.idle == IdleBehavior::ManualColor ? L"manual" : L"armourycrate");
    WriteConfigValue(ini, L"App", L"StartMinimized", p.startMinimized ? L"1" : L"0");
    std::string recent;
    for (size_t i = 0; i < p.recentColors.size(); ++i) recent += (i ? "," : "") + ToHex(p.recentColors[i]);
    WriteConfigValue(ini, L"App", L"RecentColors", Widen(recent));

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
