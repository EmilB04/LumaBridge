#include "armoury_crate.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cwctype>

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

void HandBackLighting() {
    const int code = RunHidden(Schtasks(L"/run"), 10000);
    if (code == 0) {
        LUMA_INFO("hand-back: started the hand-back task (details in %%ProgramData%%\\LumaBridge\\handback.log)");
        return;
    }
    LUMA_INFO("hand-back: couldn't start the hand-back task (schtasks exit %d; not set up?) - opening Armoury "
              "Crate instead", code);
    LaunchArmouryCrate();
}

}  // namespace luma::app
