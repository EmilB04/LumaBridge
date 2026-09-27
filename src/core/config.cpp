#include "config.h"

#include <windows.h>

#include <iterator>

#include <cwchar>
#include <cwctype>

namespace luma {
namespace {

std::wstring ReadString(const std::wstring& ini, const wchar_t* section, const wchar_t* key,
                        const wchar_t* def) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(section, key, def, buf, static_cast<DWORD>(std::size(buf)),
                             ini.c_str());
    return buf;
}

std::wstring Trim(std::wstring s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    s = s.substr(b, e - b);
    // Tolerate quoted values: RealDllPath="C:\Program Files\..."
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    return s;
}

double ReadNumber(const std::wstring& ini, const wchar_t* section, const wchar_t* key,
                  double def) {
    std::wstring s = Trim(ReadString(ini, section, key, L""));
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    double v = std::wcstod(s.c_str(), &end);
    return end == s.c_str() ? def : v;
}

bool ReadBool(const std::wstring& ini, const wchar_t* section, const wchar_t* key, bool def) {
    std::wstring s = Trim(ReadString(ini, section, key, L""));
    if (s.empty()) return def;
    return s == L"1" || _wcsicmp(s.c_str(), L"true") == 0 || _wcsicmp(s.c_str(), L"yes") == 0 ||
           _wcsicmp(s.c_str(), L"on") == 0;
}

std::vector<std::wstring> SplitList(const std::wstring& s, wchar_t sep = L',') {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(sep, start);
        if (comma == std::wstring::npos) comma = s.size();
        std::wstring item = Trim(s.substr(start, comma - start));
        if (!item.empty()) out.push_back(item);
        start = comma + 1;
    }
    return out;
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

std::wstring LocalAppDataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(buf) + L"\\LumaBridge";
}

std::wstring UserConfigPath() {
    std::wstring dir = LocalAppDataDir();
    return dir.empty() ? L"" : dir + L"\\LumaBridge.ini";
}

bool WriteConfigValue(const std::wstring& iniPath, const wchar_t* section, const wchar_t* key,
                      const std::wstring& value) {
    size_t sep = iniPath.find_last_of(L"\\/");
    if (sep != std::wstring::npos) CreateDirectoryW(iniPath.substr(0, sep).c_str(), nullptr);
    return WritePrivateProfileStringW(section, key, value.c_str(), iniPath.c_str()) != 0;
}

bool ParseAuraDeviceType(const std::wstring& token, uint32_t* out) {
    // Values from the Aura SDK's AuraSyncDeviceType definitions.
    static const struct {
        const wchar_t* name;
        uint32_t type;
    } kNames[] = {
        {L"motherboard", 0x00010000}, {L"motherboard_ledstrip", 0x00011000},
        {L"aio", 0x00020000},         {L"vga", 0x00030000},
        {L"gpu", 0x00030000},         {L"display", 0x00040000},
        {L"headset", 0x00050000},     {L"microphone", 0x00060000},
        {L"hdd", 0x00070000},         {L"bd", 0x00080000},
        {L"dram", 0x00090000},        {L"ram", 0x00090000},
        {L"keyboard", 0x000A0000},    {L"nb_keyboard", 0x000A0001},
        {L"nb_keyboard_4zone", 0x000A0011}, {L"mouse", 0x000B0000},
        {L"chassis", 0x000C0000},     {L"projector", 0x000D0000},
    };
    for (const auto& n : kNames) {
        if (_wcsicmp(token.c_str(), n.name) == 0) {
            *out = n.type;
            return true;
        }
    }
    wchar_t* end = nullptr;
    unsigned long v = std::wcstoul(token.c_str(), &end, 0);
    if (end != token.c_str() && *end == L'\0') {
        *out = static_cast<uint32_t>(v);
        return true;
    }
    return false;
}

Config LoadConfig(const std::wstring& moduleDir) {
    Config cfg;

    std::vector<std::wstring> candidates;
    if (!moduleDir.empty()) candidates.push_back(moduleDir + L"\\LumaBridge.ini");
    std::wstring appData = LocalAppDataDir();
    if (!appData.empty()) candidates.push_back(appData + L"\\LumaBridge.ini");

    for (const auto& c : candidates) {
        if (FileExists(c)) {
            cfg.sourcePath = c;
            break;
        }
    }

    if (!appData.empty()) cfg.logFile = appData + L"\\lumabridge.log";
    if (cfg.sourcePath.empty()) return cfg;
    const std::wstring& ini = cfg.sourcePath;

    cfg.passthrough = ReadBool(ini, L"Logitech", L"Passthrough", cfg.passthrough);
    cfg.realDllPath = Trim(ReadString(ini, L"Logitech", L"RealDllPath", L""));

    cfg.auraEnabled = ReadBool(ini, L"Aura", L"Enabled", cfg.auraEnabled);
    cfg.auraUseSdk = _wcsicmp(Trim(ReadString(ini, L"Aura", L"Backend", L"usb")).c_str(), L"sdk") == 0;
    cfg.maxUpdateHz =
        static_cast<int>(ReadNumber(ini, L"Aura", L"MaxUpdateHz", cfg.maxUpdateHz));
    if (cfg.maxUpdateHz < 1) cfg.maxUpdateHz = 1;
    if (cfg.maxUpdateHz > 120) cfg.maxUpdateHz = 120;
    for (const auto& t : SplitList(ReadString(ini, L"Aura", L"DeviceTypes", L""))) {
        uint32_t type;
        if (_wcsicmp(t.c_str(), L"all") == 0) {
            cfg.auraDeviceTypes.clear();
            break;
        }
        if (ParseAuraDeviceType(t, &type)) cfg.auraDeviceTypes.push_back(type);
    }
    cfg.auraExcludeNames = SplitList(ReadString(ini, L"Aura", L"ExcludeNames", L""));
    cfg.auraDirectFromGames = ReadBool(ini, L"Aura", L"DirectFromGames", cfg.auraDirectFromGames);
    cfg.auraDisabledDevices = SplitList(ReadString(ini, L"Aura", L"DisabledDevices", L""), L'|');
    cfg.releaseControlOnShutdown =
        ReadBool(ini, L"Aura", L"ReleaseControlOnShutdown", cfg.releaseControlOnShutdown);

    // Percentages in the file, fractions in memory.
    cfg.auraCorrection.brightness = ReadNumber(ini, L"Color", L"Brightness", 100.0) / 100.0;
    cfg.auraCorrection.gainR = ReadNumber(ini, L"Color", L"GainR", 100.0) / 100.0;
    cfg.auraCorrection.gainG = ReadNumber(ini, L"Color", L"GainG", 100.0) / 100.0;
    cfg.auraCorrection.gainB = ReadNumber(ini, L"Color", L"GainB", 100.0) / 100.0;
    cfg.auraCorrection.gamma = ReadNumber(ini, L"Color", L"Gamma", 1.0);

    std::wstring reduce = Trim(ReadString(ini, L"Mirror", L"BitmapMode", L"average"));
    cfg.bitmapReduce = _wcsicmp(reduce.c_str(), L"brightest") == 0 ? BitmapReduce::Brightest
                                                                    : BitmapReduce::Average;

    cfg.chromaAmbientSource = Trim(ReadString(ini, L"Chroma", L"AmbientSource", L"auto"));
    cfg.chromaReportDevicesConnected = ReadBool(ini, L"Chroma", L"ReportDevicesConnected",
                                                cfg.chromaReportDevicesConnected);

    cfg.gameSenseEnabled = ReadBool(ini, L"GameSense", L"Enabled", cfg.gameSenseEnabled);
    cfg.gameSensePort = static_cast<int>(ReadNumber(ini, L"GameSense", L"Port", cfg.gameSensePort));
    if (cfg.gameSensePort < 0 || cfg.gameSensePort > 65535) cfg.gameSensePort = 49713;
    cfg.gameSenseCoreProps = Trim(ReadString(ini, L"GameSense", L"CorePropsPath", L""));
    cfg.gameSenseForwardToGG = ReadBool(ini, L"GameSense", L"ForwardToGG", cfg.gameSenseForwardToGG);

    std::wstring logFile = Trim(ReadString(ini, L"Log", L"File", L""));
    if (!logFile.empty()) cfg.logFile = logFile;
    cfg.logLevel = log::ParseLevel(Trim(ReadString(ini, L"Log", L"Level", L"info")), cfg.logLevel);

    return cfg;
}

}  // namespace luma
