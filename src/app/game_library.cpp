#include "game_library.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>

#include "game_catalog.h"
#include "game_detector.h"
#include "json.h"
#include "log.h"

namespace luma::app {
namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string ReadFileText(const std::wstring& path) {
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < (4u << 20)) out.append(buf, n);
    fclose(f);
    return out;
}

bool IsDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFile(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Slashes(std::wstring p) {
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

std::wstring Parent(const std::wstring& p) { return p.substr(0, p.find_last_of(L"\\/")); }

std::wstring Stem(const std::wstring& p) {
    std::wstring n = p.substr(p.find_last_of(L"\\/") + 1);
    const size_t dot = n.find_last_of(L'.');
    return dot == std::wstring::npos ? n : n.substr(0, dot);
}

std::wstring RegString(HKEY root, const wchar_t* sub, const wchar_t* value) {
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, sub, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    return buf;
}

class Collector {
public:
    void Add(InstalledGame g) {
        g.dir = Slashes(g.dir);
        if (g.dir.empty() || (!g.manual && !IsDir(g.dir))) return;
        g.name = games::CleanName(g.name);
        if (g.name.empty()) g.name = g.dir.substr(g.dir.find_last_of(L'\\') + 1);
        const std::wstring key = games::Lower(g.manual ? g.exePath : g.dir);
        if (!g.manual) {
            // Already listed (also as a game the user added), or inside a listed game's folder.
            const std::wstring dir = games::Lower(g.dir);
            for (const auto& [k, existing] : byKey_) {
                const std::wstring other = games::Lower(existing.dir);
                if (dir == other || dir.rfind(other + L"\\", 0) == 0) return;
            }
        }
        byKey_.emplace(key, std::move(g));
    }
    std::vector<InstalledGame> Take() {
        std::vector<InstalledGame> out;
        for (auto& [k, g] : byKey_) out.push_back(std::move(g));
        return out;
    }

private:
    std::map<std::wstring, InstalledGame> byKey_;
};

void ScanSteam(Collector* c) {
    std::wstring steam = Slashes(RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"));
    if (steam.empty()) return;
    std::vector<std::wstring> libraries{steam};
    for (const auto& p : games::VdfValues(ReadFileText(steam + L"\\steamapps\\libraryfolders.vdf"), "path"))
        libraries.push_back(Slashes(Widen(p)));
    std::sort(libraries.begin(), libraries.end(),
              [](const std::wstring& a, const std::wstring& b) { return games::Lower(a) < games::Lower(b); });
    libraries.erase(std::unique(libraries.begin(), libraries.end(),
                                [](const std::wstring& a, const std::wstring& b) { return games::Lower(a) == games::Lower(b); }),
                    libraries.end());
    for (const auto& lib : libraries) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((lib + L"\\steamapps\\appmanifest_*.acf").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::string acf = ReadFileText(lib + L"\\steamapps\\" + fd.cFileName);
            const auto names = games::VdfValues(acf, "name");
            const auto dirs = games::VdfValues(acf, "installdir");
            if (dirs.empty()) continue;
            InstalledGame g;
            g.name = Widen(names.empty() ? dirs[0] : names[0]);
            g.store = L"Steam";
            g.dir = lib + L"\\steamapps\\common\\" + Widen(dirs[0]);
            // Steam's own runtimes and redistributables aren't games.
            if (!games::ClassifyPath(g.dir + L"\\x.exe").inLibrary) continue;
            const std::wstring n = games::Lower(g.name);
            if (n.find(L"redistributable") != std::wstring::npos || n.find(L"steam linux runtime") != std::wstring::npos ||
                n.find(L"proton") == 0)
                continue;
            c->Add(std::move(g));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void ScanEpic(Collector* c) {
    wchar_t pd[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) return;
    const std::wstring dir = std::wstring(pd) + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.item").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        Json j;
        if (!Json::Parse(ReadFileText(dir + L"\\" + fd.cFileName), &j) || !j.IsObject()) continue;
        InstalledGame g;
        g.name = Widen(j["DisplayName"].String());
        g.store = L"Epic Games";
        g.dir = Widen(j["InstallLocation"].String());
        const std::string exe = j["LaunchExecutable"].String();
        if (!exe.empty()) g.exePath = Slashes(g.dir) + L"\\" + Slashes(Widen(exe));
        c->Add(std::move(g));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// "Installed apps" entries whose folder is in a game library (EA, Ubisoft, GOG, Riot, ...).
void ScanUninstallKeys(Collector* c) {
    struct Hive {
        HKEY root;
        REGSAM view;
    } hives[] = {{HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY}, {HKEY_CURRENT_USER, 0}};
    for (const auto& hive : hives) {
        HKEY root;
        if (RegOpenKeyExW(hive.root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                          KEY_READ | hive.view, &root) != ERROR_SUCCESS)
            continue;
        wchar_t sub[256];
        for (DWORD i = 0;; ++i) {
            DWORD len = static_cast<DWORD>(std::size(sub));
            if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            const std::wstring loc = Slashes(RegString(root, sub, L"InstallLocation"));
            if (loc.empty()) continue;
            const games::PathInfo where = games::ClassifyPath(loc + L"\\x.exe");
            if (!where.inLibrary) continue;
            InstalledGame g;
            g.name = RegString(root, sub, L"DisplayName");
            g.store = where.store;
            g.dir = loc;
            c->Add(std::move(g));
        }
        RegCloseKey(root);
    }
}

void ScanXbox(Collector* c) {
    const DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(drives & (1u << d))) continue;
        const std::wstring rootPath = std::wstring(1, static_cast<wchar_t>(L'A' + d)) + L":\\";
        if (GetDriveTypeW(rootPath.c_str()) != DRIVE_FIXED) continue;
        const std::wstring lib = rootPath + L"XboxGames";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((lib + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            if (_wcsicmp(fd.cFileName, L"GameSave") == 0) continue;
            InstalledGame g;
            g.name = fd.cFileName;
            g.store = L"Xbox";
            g.dir = lib + L"\\" + fd.cFileName;
            c->Add(std::move(g));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void ScanGameBar(Collector* c) {
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return;
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = static_cast<DWORD>(std::size(sub));
        if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring exe = RegString(root, sub, L"MatchedExeFullPath");
        if (exe.rfind(L"\\\\?\\", 0) == 0) exe = exe.substr(4);
        if (exe.empty() || !IsFile(exe)) continue;
        const games::PathInfo where = games::ClassifyPath(exe);
        InstalledGame g;
        g.exePath = exe;
        g.name = ExeProductName(exe);
        if (g.name.empty()) g.name = Stem(exe);
        g.store = where.inLibrary ? where.store : L"Windows";
        g.dir = Parent(exe);
        c->Add(std::move(g));
    }
    RegCloseKey(root);
}

// File names only: lighting SDK DLLs the game ships, and its exe names.
void LookInside(InstalledGame* g) {
    int budget = 6000;
    std::vector<std::pair<std::wstring, int>> stack{{g->dir, 0}};
    while (!stack.empty() && budget > 0) {
        auto [dir, depth] = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (--budget <= 0) break;
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (depth < 4 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back({dir + L"\\" + name, depth + 1});
                continue;
            }
            const std::wstring lower = games::Lower(name);
            if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".dll") == 0) {
                if (g->sdk.empty())
                    if (const char* sdk = games::LightingSdkForModule(name)) g->sdk = sdk;
            } else if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".exe") == 0) {
                if (!games::IsHelperExe(name)) g->exeNames.push_back(Utf8(lower));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

}  // namespace

std::vector<InstalledGame> ScanInstalledGames(const std::vector<std::wstring>& manualExes) {
    const uint64_t start = GetTickCount64();
    Collector c;
    for (const auto& exe : manualExes) {
        InstalledGame g;
        g.manual = true;
        g.exePath = exe;
        g.dir = Parent(exe);
        g.name = IsFile(exe) ? ExeProductName(exe) : L"";
        if (g.name.empty()) g.name = Stem(exe);
        g.store = L"Added by you";
        c.Add(std::move(g));
    }
    ScanSteam(&c);
    ScanEpic(&c);
    ScanUninstallKeys(&c);
    ScanXbox(&c);
    ScanGameBar(&c);
    std::vector<InstalledGame> out = c.Take();
    for (auto& g : out) {
        if (IsDir(g.dir)) LookInside(&g);
        if (!g.exePath.empty()) {
            const std::wstring n = games::Lower(g.exePath.substr(g.exePath.find_last_of(L"\\/") + 1));
            g.exeNames.push_back(Utf8(n));
        }
    }
    std::sort(out.begin(), out.end(), [](const InstalledGame& a, const InstalledGame& b) {
        return games::Lower(a.name) < games::Lower(b.name);
    });
    LUMA_INFO("games list: %d game(s) found in %llu ms", static_cast<int>(out.size()),
              static_cast<unsigned long long>(GetTickCount64() - start));
    return out;
}

}  // namespace luma::app
