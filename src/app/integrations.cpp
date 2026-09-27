#include "integrations.h"

#include "armoury_crate.h"
#include "config.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstring>
#include <iterator>

namespace luma::app {
namespace {

constexpr wchar_t kLogiClsidKey[] =
    L"SOFTWARE\\Classes\\CLSID\\{a6519e67-7632-4375-afdf-caa889744403}\\ServerBinary";

std::wstring ReadDefault(HKEY root, const wchar_t* path, REGSAM view) {
    HKEY key;
    if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &key) != ERROR_SUCCESS) return L"";
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    std::wstring out;
    if (RegQueryValueExW(key, nullptr, nullptr, nullptr, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS)
        out = buf;
    RegCloseKey(key);
    return out;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Byte-for-byte comparison: is the System32 copy our build?
bool SameFile(const std::wstring& a, const std::wstring& b) {
    HANDLE fa = CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (fa == INVALID_HANDLE_VALUE) return false;
    HANDLE fb = CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (fb == INVALID_HANDLE_VALUE) {
        CloseHandle(fa);
        return false;
    }
    bool same = true;
    LARGE_INTEGER sa{}, sb{};
    GetFileSizeEx(fa, &sa);
    GetFileSizeEx(fb, &sb);
    if (sa.QuadPart != sb.QuadPart) same = false;
    std::vector<char> ba(65536), bb(65536);
    while (same) {
        DWORD ra = 0, rb = 0;
        if (!ReadFile(fa, ba.data(), static_cast<DWORD>(ba.size()), &ra, nullptr) ||
            !ReadFile(fb, bb.data(), static_cast<DWORD>(bb.size()), &rb, nullptr) || ra != rb) {
            same = false;
            break;
        }
        if (ra == 0) break;
        if (memcmp(ba.data(), bb.data(), ra) != 0) same = false;
    }
    CloseHandle(fa);
    CloseHandle(fb);
    return same;
}

std::wstring SystemDir() {
    wchar_t buf[MAX_PATH];
    UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    return n ? std::wstring(buf, n) : L"C:\\Windows\\System32";
}

}  // namespace

std::wstring AppDirectory() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/"));
}

std::wstring PickFolder(HWND owner, const wchar_t* title) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(&dlg))))
        return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

std::wstring PickExe(HWND owner, const wchar_t* title) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(&dlg))))
        return result;
    const COMDLG_FILTERSPEC types[] = {{L"Programs (*.exe)", L"*.exe"}};
    dlg->SetFileTypes(1, types);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

Integrations::~Integrations() {
    if (worker_.joinable()) worker_.join();
}

void Integrations::Refresh(bool gsRunning, int gsPort, bool gsOk, bool foundGG, int ggPort, bool ggOk) {
    appDir_ = AppDirectory();
    const std::wstring sys = SystemDir();
    items_.clear();

    // Logitech LIGHTSYNC: registry redirect of the LED SDK's ServerBinary.
    {
        Integration it{"logitech", "Logitech LIGHTSYNC",
                       "Battlefield 1 and other LIGHTSYNC games. Your Logitech devices keep working.",
                       IntegrationState::NotInstalled, ""};
        const std::wstring proxy = appDir_ + L"\\integrations\\LumaBridge_x64.dll";
        const std::wstring user = ReadDefault(HKEY_CURRENT_USER, kLogiClsidKey, KEY_WOW64_64KEY);
        const std::wstring machine = ReadDefault(HKEY_LOCAL_MACHINE, kLogiClsidKey, KEY_WOW64_64KEY);
        if (!user.empty() && _wcsicmp(user.c_str(), proxy.c_str()) == 0) {
            it.state = IntegrationState::Active;
            it.detail = "Active for your user account";
        } else if (!machine.empty() && _wcsicmp(machine.c_str(), proxy.c_str()) == 0) {
            it.state = IntegrationState::Active;
            it.detail = "Active for all users";
        } else {
            it.detail = machine.empty() ? "G HUB not found - games will still light your Aura devices"
                                        : "G HUB found";
        }
        items_.push_back(it);
    }

    // DLLs that games load from System32.
    auto systemDll = [&](const char* id, const char* name, const char* desc, const wchar_t* dll,
                         const char* vendorRuntime) {
        Integration it{id, name, desc, IntegrationState::NotInstalled, ""};
        const std::wstring installed = sys + L"\\" + dll;
        const std::wstring ours = appDir_ + L"\\integrations\\" + dll;
        if (Exists(installed)) {
            if (SameFile(installed, ours)) {
                it.state = IntegrationState::Active;
                it.detail = "Active for all games";
            } else {
                it.state = IntegrationState::Conflict;
                it.detail = std::string(vendorRuntime) + " is installed - replacing it is optional";
            }
        } else {
            it.detail = "Needs administrator approval once";
        }
        items_.push_back(it);
    };
    systemDll("chroma", "Razer Chroma", "The largest catalogue of RGB-enabled games.", L"RzChromaSDK64.dll",
              "Razer Synapse's Chroma runtime");

    // SteelSeries GameSense: built into the app.
    {
        Integration it{"gamesense", "SteelSeries GameSense", "Games that support SteelSeries GG / Engine.",
                       IntegrationState::Active, ""};
        if (!gsRunning) {
            it.state = IntegrationState::NotInstalled;
            it.detail = "Turned off";
        } else if (!gsOk) {
            it.state = IntegrationState::Problem;
            it.detail = "Games can't find LumaBridge - click Repair (administrator, once)";
        } else if (ggPort && ggOk) {
            it.detail = "Built in (port " + std::to_string(gsPort) +
                        "), passing everything on to SteelSeries GG - Moments keeps working";
        } else if (ggPort) {
            it.state = IntegrationState::Conflict;
            it.detail = "Built in (port " + std::to_string(gsPort) +
                        "); SteelSeries GG isn't answering right now - start GG for Moments";
        } else {
            it.detail = "Built in, listening on port " + std::to_string(gsPort) +
                        (foundGG ? " (forwarding to SteelSeries GG is off)" : "");
        }
        items_.push_back(it);
    }

    items_.push_back(Integration{"corsair", "Corsair iCUE",
                                 "Games ship their own iCUE file, so add LumaBridge to each game folder.",
                                 IntegrationState::PerGame, "Per game"});

    {
        Integration it{"handback", "Armoury Crate hand-back",
                       "Lets LumaBridge give the lights back to Armoury Crate silently (no window, no clicks) by "
                       "restarting the motherboard's lighting controller, which then reloads Armoury Crate's effect.",
                       IntegrationState::NotInstalled, ""};
        if (HandbackTaskInstalled()) {
            DWORD version = 0, size = sizeof version;
            RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LumaBridge", L"HandbackTaskVersion", RRF_RT_REG_DWORD,
                         nullptr, &version, &size);
            if (version >= kHandbackTaskVersion) {
                it.state = IntegrationState::Active;
                it.detail = "Set up";
            } else {
                it.state = IntegrationState::Problem;
                it.detail = "Set up by an older LumaBridge - click Update (administrator, once)";
            }
        } else {
            it.detail = "Needs administrator approval once. Without it, LumaBridge opens Armoury Crate instead.";
        }
        items_.push_back(it);
    }

    systemDll("lightfx", "Alienware AlienFX", "Older titles with Alienware lighting.", L"LightFX.dll",
              "Alienware Command Center");
}

void Integrations::Install(const std::string& id, const std::wstring& gameDir, bool force) {
    if (id == "logitech") {
        Run(id, "Logitech LIGHTSYNC set up", L"Install-LogiLedProxy.ps1", L"", false);
    } else if (id == "handback") {
        Run(id, "Silent hand-back set up", L"Install-HandbackTask.ps1", L"", true);
    } else if (id == "gamesense") {
        Run(id, "GameSense folder repaired", L"Install-SdkEmulators.ps1", L"-Sdk GameSense", true);
    } else {
        std::wstring sdk = id == "chroma" ? L"Chroma" : id == "lightfx" ? L"LightFX" : L"Corsair";
        std::wstring args = L"-Sdk " + sdk;
        if (!gameDir.empty()) args += L" -GameDir \"" + gameDir + L"\"";
        if (force) args += L" -Force";
        Run(id, id == "corsair" ? "Corsair iCUE added to the game" : "Installed", L"Install-SdkEmulators.ps1",
            args, gameDir.empty());
    }
}

void Integrations::Remove(const std::string& id, const std::wstring& gameDir) {
    if (id == "logitech") {
        Run(id, "Logitech LIGHTSYNC removed", L"Install-LogiLedProxy.ps1", L"-Uninstall", false);
        return;
    }
    if (id == "handback") {
        Run(id, "Silent hand-back removed", L"Install-HandbackTask.ps1", L"-Uninstall", true);
        return;
    }
    std::wstring sdk = id == "chroma" ? L"Chroma" : id == "lightfx" ? L"LightFX" : L"Corsair";
    std::wstring args = L"-Sdk " + sdk + L" -Uninstall";
    if (!gameDir.empty()) args += L" -GameDir \"" + gameDir + L"\"";
    Run(id, "Removed", L"Install-SdkEmulators.ps1", args, gameDir.empty());
}

std::string Integrations::LastMessage() const {
    std::lock_guard<std::mutex> lock(msgMutex_);
    return message_;
}

std::string Integrations::LastId() const {
    std::lock_guard<std::mutex> lock(msgMutex_);
    return lastId_;
}

namespace {

std::wstring Base64Utf16(const std::wstring& text) {
    static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes;
    for (wchar_t c : text) {
        bytes.push_back(static_cast<unsigned char>(c & 0xFF));
        bytes.push_back(static_cast<unsigned char>((c >> 8) & 0xFF));
    }
    std::wstring out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned v = bytes[i] << 16 | (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) |
                           (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out += kChars[(v >> 18) & 63];
        out += kChars[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? kChars[(v >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kChars[v & 63] : '=';
    }
    return out;
}

std::wstring Quote(const std::wstring& s) {  // PowerShell single-quoted string
    std::wstring out = L"'";
    for (wchar_t c : s) out += c == L'\'' ? std::wstring(L"''") : std::wstring(1, c);
    return out + L"'";
}

}  // namespace

void Integrations::Run(const std::string& id, const std::string& what, const std::wstring& script,
                       const std::wstring& args, bool elevated) {
    if (busy_) return;
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    {
        std::lock_guard<std::mutex> lock(msgMutex_);
        lastId_ = id;
        message_ = elevated ? "Waiting for you to approve the Windows administrator prompt..." : "Working...";
    }
    // Everything the script prints goes to %LOCALAPPDATA%\LumaBridge\setup.log, so a failure
    // can be looked at. -EncodedCommand avoids any quoting trouble with paths.
    const std::wstring log = LocalAppDataDir() + L"\\setup.log";
    std::wstring command =
        L"$ErrorActionPreference = 'Continue'; $global:LASTEXITCODE = 0; "
        L"'==== ' + (Get-Date -Format s) + ' ' + " + Quote(script + L" " + args) + L" | Out-File -Append -Encoding utf8 " + Quote(log) + L"; "
        L"try { & " + Quote(appDir_ + L"\\scripts\\" + script) + L" " + args +
        L" *>&1 | Out-File -Append -Encoding utf8 " + Quote(log) + L"; exit $LASTEXITCODE } "
        L"catch { $_ | Out-String | Out-File -Append -Encoding utf8 " + Quote(log) + L"; exit 1 }";
    std::wstring params = L"-NoProfile -ExecutionPolicy Bypass -EncodedCommand " + Base64Utf16(command);
    const HWND owner = owner_;
    worker_ = std::thread([this, what, params, elevated, owner] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.hwnd = owner;  // the administrator prompt opens in front of LumaBridge
        sei.lpVerb = elevated ? L"runas" : L"open";
        sei.lpFile = L"powershell.exe";
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        std::string msg;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) {
            msg = GetLastError() == ERROR_CANCELLED ? "Cancelled - the administrator prompt was declined"
                                                    : "Could not start PowerShell";
        } else {
            WaitForSingleObject(sei.hProcess, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
            msg = code == 0 ? "Done: " + what
                            : "Failed (exit code " + std::to_string(code) + ") - details in setup.log (Settings > Open log folder)";
        }
        CoUninitialize();
        {
            std::lock_guard<std::mutex> lock(msgMutex_);
            message_ = msg;
        }
        busy_ = false;
        finished_ = true;
    });
}

}  // namespace luma::app
