// What counts as a game, and which lighting SDK a loaded DLL belongs to. Pure C++ (the
// Windows side is game_detector.cpp), unit tested.
//
// A running program is a game when its exe lives in a game store's library (Steam, Epic,
// EA, Ubisoft, GOG, Xbox, Riot, ...) or Windows itself lists it as a game (the Game Bar's
// GameConfigStore), and it has a real window. Helpers in those folders (crash reporters,
// anti-cheat, launchers, installers) are skipped.
#pragma once

#include <cctype>
#include <cwctype>
#include <string>
#include <vector>

namespace luma::app::games {

inline std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

struct PathInfo {
    bool inLibrary = false;  // under a known game library
    std::wstring store;      // "Steam", "Epic Games", ...
    std::wstring folder;     // the game's folder in that library ("Rocket League")
};

// Classifies an exe path by the game library it's in.
inline PathInfo ClassifyPath(const std::wstring& exePath) {
    struct Root {
        const wchar_t* marker;  // lower case, with both separators
        const wchar_t* store;
    };
    static const Root kRoots[] = {
        {L"\\steamapps\\common\\", L"Steam"},
        {L"\\epic games\\", L"Epic Games"},
        {L"\\ea games\\", L"EA"},
        {L"\\origin games\\", L"EA"},
        {L"\\ubisoft game launcher\\games\\", L"Ubisoft Connect"},
        {L"\\gog galaxy\\games\\", L"GOG"},
        {L"\\gog games\\", L"GOG"},
        {L"\\xboxgames\\", L"Xbox"},
        {L"\\riot games\\", L"Riot"},
        {L"\\battle.net\\games\\", L"Battle.net"},
        {L"\\rockstar games\\", L"Rockstar"},
        {L"\\amazon games\\library\\", L"Amazon Games"},
    };
    // Folders in those libraries that aren't games.
    static const wchar_t* kNotGames[] = {
        L"launcher", L"directxredist", L"steamworks shared", L"epic online services", L"riot client",
        L"vcredist", L"_commonredist", L"steamvr", L"wallpaper_engine", L"ea desktop", L"ea app",
        L"rockstar games launcher", L"social club",
    };
    PathInfo info;
    std::wstring p = Lower(exePath);
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    for (const Root& r : kRoots) {
        const size_t at = p.find(r.marker);
        if (at == std::wstring::npos) continue;
        const size_t start = at + std::wstring(r.marker).size();
        const size_t end = p.find(L'\\', start);
        if (end == std::wstring::npos) return info;  // an exe directly in the library root
        const std::wstring folderLower = p.substr(start, end - start);
        for (const wchar_t* n : kNotGames)
            if (folderLower == n) return info;
        info.inLibrary = true;
        info.store = r.store;
        info.folder = exePath.substr(start, end - start);  // original case
        return info;
    }
    return info;
}

// Helper programs that live next to games but aren't the game.
inline bool IsHelperExe(const std::wstring& exeName) {
    static const wchar_t* kParts[] = {
        L"crash",  L"report",    L"easyanticheat", L"eac_",     L"beservice", L"battleye", L"setup",
        L"install", L"redist",   L"dxsetup",       L"prereq",   L"uninst",    L"updater",  L"cefprocess",
        L"cefsharp", L"webhelper", L"overlay",     L"launcher", L"bootstrap", L"helper",   L"anticheat",
        L"vc_redist", L"unins000", L"dotnet",      L"serviceui",
    };
    const std::wstring n = Lower(exeName);
    for (const wchar_t* part : kParts)
        if (n.find(part) != std::wstring::npos) return true;
    return false;
}

// Anti-cheat processes and folders. LumaBridge never opens a game's memory (to list its
// DLLs) while one of these is involved: only what the game sends counts then.
inline bool IsAntiCheatName(const std::wstring& name) {
    static const wchar_t* kParts[] = {L"easyanticheat", L"battleye", L"beservice", L"vgc", L"vanguard",
                                      L"eaanticheat", L"faceit", L"ricochet", L"xigncode", L"nprotect"};
    const std::wstring n = Lower(name);
    for (const wchar_t* part : kParts)
        if (n.find(part) != std::wstring::npos) return true;
    return false;
}

// The lighting SDK a DLL name belongs to (games load these to talk to RGB software), or
// nullptr. The LumaBridge DLLs themselves count: the game is using one of our emulators.
inline const char* LightingSdkForModule(const std::wstring& moduleName) {
    const std::wstring n = Lower(moduleName);
    auto starts = [&](const wchar_t* p) { return n.rfind(p, 0) == 0; };
    if (starts(L"logitechled") || starts(L"logiled") || starts(L"lumabridge_x")) return "Logitech LIGHTSYNC";
    if (starts(L"rzchromasdk") || starts(L"rzchromaconnect")) return "Razer Chroma";
    if (starts(L"cuesdk") || starts(L"icuesdk")) return "Corsair iCUE";
    if (starts(L"lightfx")) return "Alienware AlienFX";
    return nullptr;
}

// Letters and digits only, lower case: "Rocket League" and "ROCKETLEAGUE" compare equal
// (GameSense names games in upper case without spaces).
inline std::string Normalize(const std::string& s) {
    std::string out;
    for (unsigned char c : s)
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
    return out;
}

// "Battlefield™ 1" -> "Battlefield 1": trademark signs clutter names and the UI font may not
// have them.
inline std::wstring CleanName(std::wstring s) {
    std::wstring out;
    for (wchar_t c : s)
        if (c != L'\u2122' && c != L'\u00AE' && c != L'\u00A9') out += c;
    // collapse double spaces left behind
    std::wstring tidy;
    for (wchar_t c : out)
        if (!(c == L' ' && !tidy.empty() && tidy.back() == L' ')) tidy += c;
    while (!tidy.empty() && tidy.back() == L' ') tidy.pop_back();
    return tidy;
}

// Product names that say nothing about which game it is.
inline bool IsGenericProductName(const std::wstring& name) {
    const std::wstring n = Lower(name);
    return n.empty() || n.find(L"unreal engine") != std::wstring::npos || n == L"unity" ||
           n.find(L"unity player") != std::wstring::npos || n.find(L"bootstrap") != std::wstring::npos ||
           n.find(L"microsoft") != std::wstring::npos || n.find(L"operating system") != std::wstring::npos ||
           n == L"game" || n == L"launcher";
}

// Does a running game do dynamic lighting?
enum class Support {
    Active,     // sending colors to LumaBridge right now
    Known,      // has sent colors before (remembered), just not at the moment
    SdkLoaded,  // loaded a lighting SDK, no colors yet (menus, loading screens)
    None,       // no sign of it
    Unknown,    // couldn't look inside the process (anti-cheat), and no colors so far
};

inline Support ClassifySupport(bool sending, bool seenBefore, bool sdkLoaded, bool modulesReadable) {
    if (sending) return Support::Active;
    if (seenBefore) return Support::Known;
    if (sdkLoaded) return Support::SdkLoaded;
    return modulesReadable ? Support::None : Support::Unknown;
}

inline bool SupportsLighting(Support s) { return s == Support::Active || s == Support::Known || s == Support::SdkLoaded; }

// ---- Installed-game manifests --------------------------------------------------------

// Values of every `"key" "value"` pair with this key (case-insensitive) in a Valve KeyValues
// text (libraryfolders.vdf, appmanifest_*.acf). Backslash escapes are undone.
inline std::vector<std::string> VdfValues(const std::string& text, const std::string& key) {
    std::vector<std::string> tokens;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        std::string tok;
        for (++i; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            tok += text[i];
        }
        tokens.push_back(tok);
    }
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string k = lower(key);
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < tokens.size(); ++i)
        if (lower(tokens[i]) == k) out.push_back(tokens[++i]);
    return out;
}

}  // namespace luma::app::games
