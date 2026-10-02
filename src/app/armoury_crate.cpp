#include "armoury_crate.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <cwctype>
#include <string>
#include <thread>

#include "log.h"

namespace luma::app {
namespace {

bool ContainsNoCase(const std::wstring& hay, const wchar_t* needle) {
    std::wstring h = hay, n = needle;
    for (auto& c : h) c = static_cast<wchar_t>(std::towlower(c));
    for (auto& c : n) c = static_cast<wchar_t>(std::towlower(c));
    return h.find(n) != std::wstring::npos;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c < 128 ? c : '?');
    return s;
}

// The shell id of the Armoury Crate entry in "shell:AppsFolder" (an AppUserModelID for the
// Store version, a path-like id for a classic install). Empty when not found.
std::wstring FindArmouryCrateAppId() {
    std::wstring exact, partial;
    IShellItem* folder = nullptr;
    if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&folder)))) return L"";
    IEnumShellItems* items = nullptr;
    if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) {
        IShellItem* item = nullptr;
        while (exact.empty() && items->Next(1, &item, nullptr) == S_OK) {
            PWSTR name = nullptr, id = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &id))) {
                const std::wstring display = name;
                if (_wcsicmp(display.c_str(), L"Armoury Crate") == 0)
                    exact = id;  // the app itself
                else if (partial.empty() && ContainsNoCase(display, L"armoury crate") &&
                         !ContainsNoCase(display, L"uninstall") && ContainsNoCase(id, L"armourycrate"))
                    partial = id;  // localised / suffixed name, used if no exact match exists
            }
            if (name) CoTaskMemFree(name);
            if (id) CoTaskMemFree(id);
            item->Release();
        }
        items->Release();
    }
    folder->Release();
    return exact.empty() ? partial : exact;
}

constexpr wchar_t kHandbackTask[] = L"\\LumaBridge\\Hand back lighting";

// Runs a console tool without a window and returns its exit code (-1 on failure/timeout).
int RunHidden(std::wstring cmdLine, DWORD timeoutMs) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi))
        return -1;
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::wstring Schtasks(const wchar_t* verb) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return L"\"" + std::wstring(sys) + L"\\schtasks.exe\" " + verb + L" /tn \"" + kHandbackTask + L"\"";
}

std::wstring HandbackLogPath() {
    wchar_t dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(dir) + L"\\LumaBridge\\handback.log";
}

long long FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (static_cast<long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

// Copies what the hand-back task wrote (from `offset` on) into the app log once it has
// finished (restarting the controller can take ~10 s), so one log shows whether it worked.
void LogHandbackResult(std::wstring path, long long offset) {
    std::thread([path, offset] {
        std::string text;
        for (int waited = 0; waited < 30000; waited += 1000) {
            Sleep(1000);
            text.clear();
            FILE* f = _wfopen(path.c_str(), L"rb");
            if (!f) continue;
            _fseeki64(f, offset, SEEK_SET);
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            fclose(f);
            // The task's last line for each controller:
            if (text.find("pnputil /restart-device") != std::string::npos ||
                text.find("not restarting") != std::string::npos || text.find("no Aura") != std::string::npos)
                break;
        }
        if (text.empty()) {
            LUMA_WARN("hand-back: the task wrote nothing within 30 s (did it run?)");
            return;
        }
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) LUMA_INFO("hand-back task: %s", line.c_str());
            start = end + 1;
        }
    }).detach();
}

}  // namespace

bool LaunchArmouryCrate() {
    const std::wstring id = FindArmouryCrateAppId();
    if (id.empty()) {
        LUMA_WARN("hand-back: Armoury Crate not found in the app list - open it yourself to restore its lighting");
        return false;
    }
    const std::wstring target = L"shell:AppsFolder\\" + id;
    HINSTANCE r = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const bool ok = reinterpret_cast<INT_PTR>(r) > 32;
    if (ok)
        LUMA_INFO("hand-back: started Armoury Crate (%s) so it re-applies its lighting", Narrow(id).c_str());
    else
        LUMA_WARN("hand-back: could not start Armoury Crate (%s), error %d", Narrow(id).c_str(),
                  static_cast<int>(reinterpret_cast<INT_PTR>(r)));
    return ok;
}


bool HandbackTaskInstalled() { return RunHidden(Schtasks(L"/query"), 10000) == 0; }

bool ArmouryCrateWindowOpen() {
    bool open = false;
    EnumWindows(
        [](HWND w, LPARAM found) -> BOOL {
            if (!IsWindowVisible(w)) return TRUE;
            wchar_t title[64] = {};
            GetWindowTextW(w, title, 64);
            if (_wcsicmp(title, L"Armoury Crate") != 0) return TRUE;
            *reinterpret_cast<bool*>(found) = true;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&open));
    return open;
}

void HandBackLighting() {
    const std::wstring log = HandbackLogPath();
    const long long before = FileSize(log);
    const int code = RunHidden(Schtasks(L"/run"), 10000);
    if (code == 0) {
        LUMA_INFO("hand-back: started the hand-back task (details in %%ProgramData%%\\LumaBridge\\handback.log)");
        LogHandbackResult(log, before);
        return;
    }
    if (HandbackTaskInstalled()) {
        // Set up, but /run was refused: it's still busy with the previous hand-back.
        LUMA_INFO("hand-back: the hand-back task is still running from the previous hand-back");
        return;
    }
    LUMA_INFO("hand-back: the hand-back task isn't set up (schtasks exit %d) - opening Armoury Crate instead", code);
    LaunchArmouryCrate();
}

}  // namespace luma::app
