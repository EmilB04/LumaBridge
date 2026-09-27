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

}  // namespace luma::app
