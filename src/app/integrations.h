// "Integrations" page: which game-lighting SDKs are hooked up, and one-click install / remove
// (wrapping the PowerShell scripts shipped next to the app, elevated when needed).
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "integration_plan.h"

namespace luma::app {

struct Integration {
    std::string id;  // "logitech", "chroma", "gamesense", "corsair", "lightfx"
    std::string name;
    std::string description;
    IntegrationState state = IntegrationState::NotInstalled;
    std::string detail;
};

class Integrations {
public:
    ~Integrations();

    void Refresh(bool gameSenseRunning, int gameSensePort, bool gameSenseOk, bool foundGG, int ggPort,
                 bool ggOk);
    const std::vector<Integration>& list() const { return items_; }

    // Runs the matching script in the background. `gameDir` only for per-game SDKs; `force`
    // replaces a vendor runtime.
    void Install(const std::string& id, const std::wstring& gameDir = L"", bool force = false);
    void Remove(const std::string& id, const std::wstring& gameDir = L"");
    // The UI pumps the queue every frame, including while another page is open.
    void SetAll(bool enabled);
    void Tick();
    bool BatchBusy() const { return batchRunning_; }

    // Writes (or removes) a game config file with administrator rights, through
    // scripts\Write-GameFile.ps1 (only for built-in game feed files).
    void WriteGameFileElevated(const std::string& id, const std::string& what, const std::wstring& path,
                               const std::string& content, bool remove = false);

    // Parent window for the administrator prompt (so it opens in front of the app).
    void SetOwner(HWND owner) { owner_ = owner; }
    bool Busy() const { return busy_ || batchRunning_; }
    // Id of the integration the last action was for ("" before any).
    std::string LastId() const;
    // One-line result of the last action ("" while none); thread-safe.
    std::string LastMessage() const;
    // True once after a background action finished (the UI then calls Refresh).
    bool TakeFinished() { return finished_.exchange(false); }

private:
    void Run(const std::string& id, const std::string& what, const std::wstring& script, const std::wstring& args,
             bool elevated);

    std::vector<Integration> items_;
    std::wstring appDir_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> finished_{false};
    mutable std::mutex msgMutex_;
    std::string message_;
    std::string lastId_;
    std::thread worker_;
    HWND owner_ = nullptr;
    std::vector<std::string> pending_;
    bool batchRunning_ = false, batchEnabled_ = false, batchWaiting_ = false;
};

// Version of the hand-back task Install-HandbackTask.ps1 registers (it records it in
// HKLM\SOFTWARE\LumaBridge\HandbackTaskVersion); an older task needs setting up again.
constexpr DWORD kHandbackTaskVersion = 7;
// Same for the hardware helper (Install-Helper.ps1, HKLM\SOFTWARE\LumaBridge\HelperTaskVersion).
constexpr DWORD kHelperTaskVersion = 7;  // 7: prevents ASUS lighting service restarts while paused

std::wstring AppDirectory();
// Folder picker; empty when cancelled.
std::wstring PickFolder(HWND owner, const wchar_t* title);
// Picks an .exe; empty when cancelled.
std::wstring PickExe(HWND owner, const wchar_t* title);

}  // namespace luma::app
