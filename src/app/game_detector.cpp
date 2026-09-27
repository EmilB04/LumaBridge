#include "game_detector.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <iterator>
#include <vector>

#include "game_catalog.h"
#include "log.h"

namespace luma::app {
namespace {

constexpr uint64_t kScanMs = 2000;
constexpr uint64_t kStoreReloadMs = 60000;
constexpr uint64_t kModuleRecheckMs = 10000;  // SDKs are often loaded after the menus appear

}  // namespace

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

namespace {

std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\0')) s.pop_back();
    while (!s.empty() && s.front() == L' ') s.erase(s.begin());
    return s;
}

}  // namespace

// ProductName, else FileDescription, from the exe's version resource.
std::wstring ExeProductName(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return L"";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"";
    struct Lang {
        WORD language, codepage;
    }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) ||
        len < sizeof(Lang))
        return L"";
    for (const wchar_t* field : {L"ProductName", L"FileDescription"}) {
        wchar_t key[96];
        swprintf(key, std::size(key), L"\\StringFileInfo\\%04x%04x\\%ls", langs[0].language, langs[0].codepage, field);
        wchar_t* value = nullptr;
        UINT vlen = 0;
        if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &vlen) && value && vlen) {
            std::wstring v = Trim(std::wstring(value, vlen));
            if (!games::IsGenericProductName(v)) return v;
        }
    }
    return L"";
}

namespace {

// Does the game's folder (or the exe's own folder) ship an anti-cheat?
bool HasAntiCheatFolder(const std::wstring& exePath, const std::wstring& gameRoot) {
    std::vector<std::wstring> dirs;
    dirs.push_back(exePath.substr(0, exePath.find_last_of(L"\\/")));
    if (!gameRoot.empty()) dirs.push_back(gameRoot);
    for (const auto& dir : dirs) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        bool found = false;
        do {
            if (games::IsAntiCheatName(fd.cFileName)) found = true;
        } while (!found && FindNextFileW(h, &fd));
        FindClose(h);
        if (found) return true;
    }
    return false;
}

BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM param) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    RECT r;
    if (!IsIconic(hwnd) && (!GetWindowRect(hwnd, &r) || r.right - r.left < 320 || r.bottom - r.top < 200)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    reinterpret_cast<std::set<uint32_t>*>(param)->insert(pid);
    return TRUE;
}

}  // namespace

void GameDetector::LoadGameConfigStore() {
    windowsGames_.clear();
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return;
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD subLen = static_cast<DWORD>(std::size(sub));
        if (RegEnumKeyExW(root, i, sub, &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t path[MAX_PATH * 2];
        DWORD bytes = sizeof path;
        if (RegGetValueW(root, sub, L"MatchedExeFullPath", RRF_RT_REG_SZ, nullptr, path, &bytes) == ERROR_SUCCESS)
        {
            std::wstring p = games::Lower(path);
            if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
            windowsGames_.insert(p);
        }
    }
    RegCloseKey(root);
}

void GameDetector::Inspect(uint32_t pid, Entry* e) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    wchar_t buf[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(buf));
    const bool ok = QueryFullProcessImageNameW(h, 0, buf, &size) != 0;
    CloseHandle(h);
    if (!ok) return;
    const std::wstring path(buf, size);

    const games::PathInfo where = games::ClassifyPath(path);
    const std::wstring lower = games::Lower(path);
    const bool windowsGame = windowsGames_.count(lower) != 0;
    const bool added = extraGames_.count(lower) != 0;
    if (!where.inLibrary && !windowsGame && !added) return;
    if (!added && games::IsHelperExe(e->exeName)) return;  // what the user added always counts

    e->candidate = true;
    RunningGame& g = e->info;
    std::wstring gameRoot;
    if (where.inLibrary) {
        const size_t at = games::Lower(path).find(games::Lower(where.folder));
        if (at != std::wstring::npos) gameRoot = path.substr(0, at + where.folder.size());
    }
    g.antiCheat = HasAntiCheatFolder(path, gameRoot);
    g.pid = pid;
    g.exe = Utf8(e->exeName);
    g.folder = Utf8(where.folder);
    g.path = path;
    g.store = added ? "Added by you" : where.inLibrary ? Utf8(where.store) : "Windows";
    std::wstring name = ExeProductName(path);
    if (name.empty()) name = where.folder;
    if (name.empty()) {
        name = e->exeName;
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name.resize(dot);
    }
    g.name = Utf8(games::CleanName(name));
}

void GameDetector::CheckModules(Entry* e, uint64_t now) {
    if (!e->info.sdk.empty() || now < e->nextModuleCheck) return;
    e->nextModuleCheck = now + kModuleRecheckMs;
    if (e->info.antiCheat || antiCheatRunning_) {
        // Opening a protected game's memory can upset its anti-cheat: don't. Whether it does
        // lighting then only shows from what it sends.
        e->info.modulesReadable = false;
        return;
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, e->info.pid);
    if (!h) {
        e->info.modulesReadable = false;
        return;
    }
    HMODULE mods[2048];
    DWORD needed = 0;
    if (EnumProcessModulesEx(h, mods, sizeof mods, &needed, LIST_MODULES_ALL)) {
        e->info.modulesReadable = true;
        const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), static_cast<DWORD>(std::size(mods)));
        for (DWORD i = 0; i < count; ++i) {
            wchar_t name[MAX_PATH];
            if (!GetModuleBaseNameW(h, mods[i], name, MAX_PATH)) continue;
            if (const char* sdk = games::LightingSdkForModule(name)) {
                e->info.sdk = sdk;
                LUMA_INFO("games: %s loaded %s (%s)", e->info.name.c_str(), Utf8(name).c_str(), sdk);
                break;
            }
        }
    } else {
        e->info.modulesReadable = false;
    }
    CloseHandle(h);
}

void GameDetector::SetExtraGames(const std::vector<std::wstring>& exePaths) {
    std::set<std::wstring> next;
    for (const auto& p : exePaths) next.insert(games::Lower(p));
    if (next == extraGames_) return;
    extraGames_ = std::move(next);
    entries_.clear();  // look at every process again
    nextScan_ = 0;
}

bool GameDetector::Poll(uint64_t now) {
    if (now < nextScan_) return false;
    nextScan_ = now + kScanMs;
    if (now >= nextStoreLoad_) {
        LoadGameConfigStore();
        nextStoreLoad_ = now + kStoreReloadMs;
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    std::set<uint32_t> alive;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    const uint32_t self = GetCurrentProcessId();
    antiCheatRunning_ = false;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        const uint32_t pid = pe.th32ProcessID;
        if (pid <= 4 || pid == self) continue;
        if (games::IsAntiCheatName(pe.szExeFile)) antiCheatRunning_ = true;
        alive.insert(pid);
        auto it = entries_.find(pid);
        if (it != entries_.end() && it->second.exeName == pe.szExeFile) continue;
        Entry e;
        e.exeName = pe.szExeFile;
        Inspect(pid, &e);
        entries_[pid] = std::move(e);
    }
    CloseHandle(snap);
    for (auto it = entries_.begin(); it != entries_.end();)
        it = alive.count(it->first) ? std::next(it) : entries_.erase(it);

    std::set<uint32_t> windowed;
    EnumWindows(&CollectWindow, reinterpret_cast<LPARAM>(&windowed));

    std::vector<RunningGame> next;
    for (auto& [pid, e] : entries_) {
        if (!e.candidate) continue;
        if (windowed.count(pid)) e.hadWindow = true;
        if (!e.hadWindow) continue;
        CheckModules(&e, now);
        next.push_back(e.info);
    }
    bool changed = next.size() != games_.size();
    for (size_t i = 0; !changed && i < next.size(); ++i)
        changed = next[i].pid != games_[i].pid || next[i].sdk != games_[i].sdk ||
                  next[i].modulesReadable != games_[i].modulesReadable;
    if (changed) {
        for (const auto& g : next) {
            bool known = false;
            for (const auto& old : games_) known |= old.pid == g.pid;
            if (!known) LUMA_INFO("games: %s is running (%s, %s)", g.name.c_str(), g.exe.c_str(), g.store.c_str());
        }
        games_ = std::move(next);
    }
    return changed;
}

}  // namespace luma::app
