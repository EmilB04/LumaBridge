// Logitech devices (G502 X Plus, ...) through Logitech's own LED SDK: LumaBridge loads
// G HUB's SDK DLL in its own process and sets the color; G HUB does the device I/O and
// takes its profile back when LumaBridge lets go (LogiLedShutdown). Nothing goes into a game.
//
// Logitech devices show one color: the effect's first LED, animated. Except that for
// breathing, color cycle and the rainbow wave, a mouse that runs those effects itself
// (G502 X Plus) gets its own effect over HID++ (logitech_hidpp.h), across all its LEDs.
// Runs on its own thread (~20 updates per second) so a slow G HUB never stalls the UI.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "effects.h"

namespace luma::app {

class LogitechOutput {
public:
    enum class State { Off, NoGHub, Waiting, Active, Released };

    ~LogitechOutput() { Stop(); }
    // `proxyPath`: LumaBridge's own Logitech proxy, which must never be loaded here.
    void Start(const std::wstring& proxyPath);
    void Stop();

    // What Logitech devices should show; `own = false` hands them back to G HUB (a game is
    // lighting them itself, or LumaBridge isn't controlling the lights).
    void Set(const fx::Params& effect, bool own);

    State state() const { return state_; }
    // Whether the mouse is running its own effect right now (not one color through G HUB).
    bool mouseEffect() const { return mouseEffect_; }
    std::wstring dllPath() const;

private:
    void Run();

    std::wstring proxyPath_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Off};
    std::atomic<bool> mouseEffect_{false};
    mutable std::mutex mutex_;
    fx::Params effect_;
    bool own_ = false;
    uint64_t effectSince_ = 0;
    std::wstring dll_;
};

}  // namespace luma::app
