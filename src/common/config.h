// LumaBridge.ini loader. See config/LumaBridge.ini.example for every key.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "color.h"
#include "log.h"

namespace luma {

struct Config {
    // [Logitech]
    bool passthrough = true;   // forward every call to the real LogiLed DLL
    std::wstring realDllPath;  // empty = auto-detect (registry, then G HUB defaults)

    // [Aura]
    bool auraEnabled = true;
    int maxUpdateHz = 30;
    // Aura device type codes to drive; empty = every device the SDK enumerates.
    std::vector<uint32_t> auraDeviceTypes;
    // Case-insensitive substrings; a device whose name contains one is skipped.
    std::vector<std::wstring> auraExcludeNames;
    bool releaseControlOnShutdown = true;
    // Let game DLLs talk to Aura themselves when the LumaBridge app isn't running. Off by
    // default: the Aura SDK runs inside the calling process, and if it crashes it takes the
    // game down with it. With it off, games only light Aura through the app.
    bool auraDirectFromGames = false;
    // Exact Aura device names switched off in the app ('|' separated in the ini).
    std::vector<std::wstring> auraDisabledDevices;

    // [Color]
    ColorCorrection auraCorrection;

    // [Mirror]
    BitmapReduce bitmapReduce = BitmapReduce::Average;

    // [Chroma] (RzChromaSDK emulator only)
    std::wstring chromaAmbientSource = L"auto";  // auto|keyboard|mouse|headset|mousepad|keypad|chromalink
    bool chromaReportDevicesConnected = true;     // QueryDevice says every device is connected

    // [GameSense] (LumaBridgeHost.exe only)
    bool gameSenseEnabled = true;
    int gameSensePort = 49713;       // 0 = let Windows pick; falls back to that if taken
    std::wstring gameSenseCoreProps;
    bool gameSenseForwardToGG = true;  // pass everything on to SteelSeries GG when installed  // empty = %PROGRAMDATA%\SteelSeries\SteelSeries Engine 3\coreProps.json

    // [Log]
    std::wstring logFile;  // empty = %LOCALAPPDATA%\LumaBridge\lumabridge.log
    log::Level logLevel = log::Level::Info;

    // Path the config was actually read from (empty if none found; defaults used).
    std::wstring sourcePath;
};

// Looks for LumaBridge.ini next to `moduleDir`, then in %LOCALAPPDATA%\LumaBridge.
Config LoadConfig(const std::wstring& moduleDir);

// Maps "motherboard", "dram", "keyboard", ... or a hex code ("0x00090000") to an Aura type.
bool ParseAuraDeviceType(const std::wstring& token, uint32_t* out);

std::wstring LocalAppDataDir();  // %LOCALAPPDATA%\LumaBridge (not created)

// The per-user config the app edits: %LOCALAPPDATA%\LumaBridge\LumaBridge.ini.
std::wstring UserConfigPath();

// Writes one key (creates the file and folder if needed).
bool WriteConfigValue(const std::wstring& iniPath, const wchar_t* section, const wchar_t* key,
                      const std::wstring& value);

}  // namespace luma
