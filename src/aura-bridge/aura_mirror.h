// Drives Aura from a LightingState on a dedicated worker thread.
//
// Game-thread entry points only take a mutex and update LightingState; the worker owns COM
// and the Aura SDK, coalesces updates, animates flash/pulse effects and pushes at most
// Config::maxUpdateHz frames per second. Aura's Apply() is an out-of-process COM call that
// can take several ms per device -- it must never run on a game's render thread.
//
// Routing: when `routeToApp` is set and the LumaBridge app is running, the worker does not
// touch Aura at all and instead sends each frame to the app (see ipc.h), which owns Aura.
// The app itself uses a mirror with routeToApp = false.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "aura_bridge.h"
#include "color.h"
#include "config.h"
#include "lighting_state.h"

namespace luma {

class AuraMirror {
public:
    struct Status {
        bool running = false;
        bool connected = false;     // talking to Aura directly
        bool routedToApp = false;   // frames go to the LumaBridge app instead
        std::vector<AuraDeviceInfo> devices;
    };

    // Starts the worker (idempotent). `sourceName` labels this front-end in the app
    // ("Logitech LIGHTSYNC", ...). The Aura connection happens asynchronously and is retried
    // every few seconds, so this succeeds even if LightingService is down.
    bool Start(const Config& cfg, HMODULE self, const char* sourceName, bool routeToApp = true);
    // Stops the worker and, if configured, releases Aura control back to Armoury Crate.
    void Stop();
    bool IsRunning() const { return thread_ != nullptr; }

    void SetStatic(Rgb c);
    void Flash(Rgb c, int durationMs, int intervalMs);
    void Pulse(Rgb c, int durationMs, int intervalMs);
    void StopEffects();
    void Save();
    void Restore();

    // Runtime settings (used by the app; the DLLs take them from the config once).
    void SetCorrection(const ColorCorrection& cc);
    // Devices (by exact Aura name) that must not be written, on top of the config filters.
    void SetDisabledDevices(const std::vector<std::wstring>& names);
    void Rescan();  // re-enumerate devices on the next worker iteration

    Status GetStatus() const;

private:
    static DWORD WINAPI ThreadMain(LPVOID self);
    void Run();
    void Wake() {
        if (wake_) SetEvent(wake_);
    }
    bool DeviceAllowed(const AuraDeviceInfo& d) const;  // needs settingsMutex_

    Config cfg_;
    std::string source_;
    bool routeToApp_ = true;
    HANDLE thread_ = nullptr;
    HANDLE wake_ = nullptr;
    HMODULE module_ = nullptr;
    std::atomic<bool> stop_{false};

    std::mutex mutex_;
    LightingState state_;  // guarded by mutex_

    mutable std::mutex settingsMutex_;  // guards everything below
    ColorCorrection correction_;
    std::vector<std::wstring> disabledDevices_;
    uint64_t settingsVersion_ = 0;
    bool rescan_ = false;
    Status status_;
};

}  // namespace luma
