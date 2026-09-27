#include "integrations.h"

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

Integrations::~Integrations() {
    if (worker_.joinable()) worker_.join();
}

void Integrations::Refresh(bool gsRunning, int gsPort, bool gsOk, bool foundGG) {
    appDir_ = AppDirectory();
    const std::wstring sys = SystemDir();
    items_.clear();

    // Logitech LIGHTSYNC: registry redirect of the LED SDK's ServerBinary.
    {
        Integration it{"logitech", "Logitech LIGHTSYNC",
                       "Battlefield 1 and other LIGHTSYNC games. Your Logitech devices keep working.",
                       IntegrationState::NotInstalled, ""};
        const std::wstring proxy = appDir_ + L"\\LumaBridge_x64.dll";
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
        const std::wstring ours = appDir_ + L"\\" + dll;
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
        } else {
            it.detail = "Built in, listening on port " + std::to_string(gsPort) +
                        (foundGG ? " (SteelSeries GG paused while LumaBridge runs)" : "");
        }
        items_.push_back(it);
    }

    items_.push_back(Integration{"corsair", "Corsair iCUE",
                                 "Games ship their own iCUE file, so add LumaBridge to each game folder.",
                                 IntegrationState::PerGame, "Per game"});

    systemDll("lightfx", "Alienware AlienFX", "Older titles with Alienware lighting.", L"LightFX.dll",
              "Alienware Command Center");
}

void Integrations::Install(const std::string& id, const std::wstring& gameDir, bool force) {
    if (id == "logitech") {
        Run("Logitech LIGHTSYNC set up", L"Install-LogiLedProxy.ps1", L"", false);
    } else if (id == "gamesense") {
        Run("GameSense folder repaired", L"Install-SdkEmulators.ps1", L"-Sdk GameSense", true);
    } else {
        std::wstring sdk = id == "chroma" ? L"Chroma" : id == "lightfx" ? L"LightFX" : L"Corsair";
        std::wstring args = L"-Sdk " + sdk;
        if (!gameDir.empty()) args += L" -GameDir \"" + gameDir + L"\"";
        if (force) args += L" -Force";
        Run(id == "corsair" ? "Corsair iCUE added to the game" : "Installed", L"Install-SdkEmulators.ps1",
            args, gameDir.empty());
    }
}

void Integrations::Remove(const std::string& id, const std::wstring& gameDir) {
    if (id == "logitech") {
        Run("Logitech LIGHTSYNC removed", L"Install-LogiLedProxy.ps1", L"-Uninstall", false);
        return;
    }
    std::wstring sdk = id == "chroma" ? L"Chroma" : id == "lightfx" ? L"LightFX" : L"Corsair";
    std::wstring args = L"-Sdk " + sdk + L" -Uninstall";
    if (!gameDir.empty()) args += L" -GameDir \"" + gameDir + L"\"";
    Run("Removed", L"Install-SdkEmulators.ps1", args, gameDir.empty());
}

std::string Integrations::LastMessage() const {
    std::lock_guard<std::mutex> lock(msgMutex_);
    return message_;
}

void Integrations::Run(const std::string& what, const std::wstring& script, const std::wstring& args,
                       bool elevated) {
    if (busy_) return;
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    {
        std::lock_guard<std::mutex> lock(msgMutex_);
        message_ = "Working...";
    }
    std::wstring params = L"-NoProfile -ExecutionPolicy Bypass -File \"" + appDir_ + L"\\" + script + L"\" " + args;
    worker_ = std::thread([this, what, params, elevated] {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = elevated ? L"runas" : L"open";
        sei.lpFile = L"powershell.exe";
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        std::string msg;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) {
            msg = GetLastError() == ERROR_CANCELLED ? "Cancelled" : "Could not start PowerShell";
        } else {
            WaitForSingleObject(sei.hProcess, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
            msg = code == 0 ? what : "Failed (see the log folder) - exit code " + std::to_string(code);
        }
        {
            std::lock_guard<std::mutex> lock(msgMutex_);
            message_ = msg;
        }
        busy_ = false;
        finished_ = true;
    });
}

}  // namespace luma::app
