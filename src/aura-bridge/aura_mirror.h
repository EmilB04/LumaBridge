// Mirrors the game's ambient LogiLed color onto Aura devices.
//
// Game-thread entry points only take a mutex and update LightingState; a dedicated worker
// thread owns COM + the Aura SDK, coalesces updates, animates flash/pulse effects and pushes
// at most Config::maxUpdateHz frames per second. Aura's Apply() is an out-of-process COM
// call that can take several ms per device -- it must never run on the game's render thread.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>

#include "color.h"
#include "config.h"
#include "lighting_state.h"

namespace luma {

class AuraMirror {
public:
    // Starts the worker (idempotent). The Aura connection itself happens asynchronously and
    // is retried every few seconds, so this succeeds even if LightingService is down.
    bool Start(const Config& cfg, HMODULE self);
    // Stops the worker and, if configured, releases Aura control back to Armoury Crate.
    void Stop();
    bool IsRunning() const { return thread_ != nullptr; }

    void SetStatic(Rgb c);
    void Flash(Rgb c, int durationMs, int intervalMs);
    void Pulse(Rgb c, int durationMs, int intervalMs);
    void StopEffects();
    void Save();
    void Restore();

private:
    static DWORD WINAPI ThreadMain(LPVOID self);
    void Run();
    void Wake() {
        if (wake_) SetEvent(wake_);
    }

    Config cfg_;
    HANDLE thread_ = nullptr;
    HANDLE wake_ = nullptr;
    HMODULE module_ = nullptr;
    std::atomic<bool> stop_{false};

    std::mutex mutex_;
    LightingState state_;  // guarded by mutex_
};

}  // namespace luma
