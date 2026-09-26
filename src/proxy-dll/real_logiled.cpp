#include "real_logiled.h"

#include <windows.h>

#include <iterator>

#include <string>
#include <vector>

#include "log.h"

namespace luma::real {

#define LUMA_DEFINE_PTR(ret, name, params, args) name##_fn name = nullptr;
LOGILED_ALL(LUMA_DEFINE_PTR)
#undef LUMA_DEFINE_PTR

namespace {

HMODULE g_module = nullptr;
std::wstring g_path;

// CLSID under which Logitech Gaming Software / G HUB register the LED SDK's DLL
// ("ServerBinary" value). LogitechLEDLib.lib reads this to find the DLL, which is also
// what the registry-redirect install mode hooks. Verify with regedit on your machine.
constexpr wchar_t kSdkClsidKey[] =
    L"SOFTWARE\\Classes\\CLSID\\{a6519e67-7632-4375-afdf-caa889744403}\\ServerBinary";

#if defined(_WIN64)
constexpr REGSAM kRegView = KEY_WOW64_64KEY;
constexpr const wchar_t* kDefaultPaths[] = {
    L"%ProgramFiles%\\LGHUB\\sdk_legacy_led_x64.dll",
    L"%ProgramFiles%\\Logitech Gaming Software\\SDK\\LED\\x64\\LogitechLed.dll",
};
#else
constexpr REGSAM kRegView = KEY_WOW64_32KEY;
constexpr const wchar_t* kDefaultPaths[] = {
    L"%ProgramW6432%\\LGHUB\\sdk_legacy_led_x86.dll",
    L"%ProgramFiles%\\LGHUB\\sdk_legacy_led_x86.dll",
    L"%ProgramW6432%\\Logitech Gaming Software\\SDK\\LED\\x86\\LogitechLed.dll",
    L"%ProgramFiles%\\Logitech Gaming Software\\SDK\\LED\\x86\\LogitechLed.dll",
};
#endif

std::wstring Expand(const std::wstring& s) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, static_cast<DWORD>(std::size(buf)));
    return (n == 0 || n > std::size(buf)) ? s : std::wstring(buf);
}

std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    return (n == 0 || n > std::size(buf)) ? p : std::wstring(buf);
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(FullPath(a).c_str(), FullPath(b).c_str()) == 0;
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Reads HKLM explicitly (not HKCR): a per-user HKCU redirect that points games at the
// proxy must not make the proxy find itself.
std::wstring ReadRegistryPath() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSdkClsidKey, 0, KEY_READ | kRegView, &key) !=
        ERROR_SUCCESS)
        return L"";
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    std::wstring out;
    if (RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) ==
            ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
        out = Expand(buf);
    RegCloseKey(key);
    return out;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? static_cast<char>(c) : '?';
    return s;
}

}  // namespace

bool IsLoaded() { return g_module != nullptr; }

std::wstring LoadedPath() { return g_path; }

bool Load(const std::wstring& configuredPath, const std::wstring& selfPath) {
    if (g_module) return true;

    std::vector<std::pair<std::wstring, const char*>> candidates;
    if (!configuredPath.empty()) candidates.emplace_back(Expand(configuredPath), "config");
    std::wstring reg = ReadRegistryPath();
    if (!reg.empty()) candidates.emplace_back(reg, "registry");
    for (const wchar_t* p : kDefaultPaths) candidates.emplace_back(Expand(p), "default");

    for (const auto& [path, source] : candidates) {
        if (!selfPath.empty() && SamePath(path, selfPath)) {
            LUMA_WARN("Logitech: skipping %s candidate %s - that is the proxy itself", source,
                      Narrow(path).c_str());
            continue;
        }
        if (!FileExists(path)) {
            LUMA_DEBUG("Logitech: %s candidate not found: %s", source, Narrow(path).c_str());
            continue;
        }
        // LOAD_WITH_ALTERED_SEARCH_PATH so the real DLL resolves its own dependencies
        // from its own folder, not the game's.
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) {
            LUMA_WARN("Logitech: LoadLibrary(%s) failed, error %lu", Narrow(path).c_str(),
                      GetLastError());
            continue;
        }
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Load), &self);
        if (m == self) {  // e.g. a hard link / junction to the proxy
            FreeLibrary(m);
            LUMA_WARN("Logitech: %s resolved to the proxy itself, skipping", Narrow(path).c_str());
            continue;
        }

        g_module = m;
        g_path = path;
        int missing = 0;
#define LUMA_RESOLVE(ret, name, params, args)                                    \
    name = reinterpret_cast<name##_fn>(                                          \
        reinterpret_cast<void*>(GetProcAddress(m, #name)));                      \
    if (!name) {                                                                 \
        ++missing;                                                               \
        LUMA_DEBUG("Logitech: real DLL has no export %s", #name);                \
    }
        LOGILED_ALL(LUMA_RESOLVE)
#undef LUMA_RESOLVE
        LUMA_INFO("Logitech: loaded real SDK from %s (%s), %d export(s) missing",
                  Narrow(path).c_str(), source, missing);
        return true;
    }

    LUMA_WARN("Logitech: real LED SDK DLL not found - running Aura-only (no pass-through)");
    return false;
}

}  // namespace luma::real
