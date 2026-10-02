// Logitech devices (G502 X Plus, ...) through Logitech's own LED SDK: LumaBridge loads
// G HUB's SDK DLL when available; supported mice also work directly over HID++ without it.
// G HUB does the device I/O for other devices and
// takes its profile back when LumaBridge lets go (LogiLedShutdown). Nothing goes into a game.
//
// Logitech devices show one color: the effect's first LED, animated. A mouse LumaBridge
// reaches directly over HID++ (logitech_hidpp.h) shows the whole effect instead: its own
// breathing, color cycle or color wave when those match, otherwise every LED its own color,
// frame by frame (the G502 X Plus, whose LED order is known).
// Runs on its own thread (~20 updates per second) so a slow G HUB never stalls the UI.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "effects.h"
#include "power_suspend.h"

namespace luma::app {

// A Logitech device found over HID++ (through its receiver or its cable).
struct LogitechDevice {
    std::string name;   // as the device reports it, e.g. "G502 X PLUS"
    int type = -1;      // HID++ device type (hidpp::DeviceTypeName), -1 if unknown
    bool rgb = false;   // has RGB lighting (hidpp::kLightingFeatures)
};

// Every Logitech device reachable over HID++, with or without lighting. Talks to each
// receiver and cable, so it takes a few seconds: run it in the background.
std::vector<LogitechDevice> ScanLogitechDevices(const std::function<bool()>& cancelled = {});

class LogitechOutput {
public:
    enum class State { Off, NoGHub, Waiting, Active, Released };

    ~LogitechOutput() { Stop(); }
    // `proxyPath`: LumaBridge's own Logitech proxy, which must never be loaded here.
    void Start(const std::wstring& proxyPath);
    void Stop();

    // What Logitech devices should show; `own = false` hands them back to G HUB (a game is
    // lighting them itself, or LumaBridge isn't controlling the lights).
    // `brightness` 0..1.
    // `inStep`: a game's lighting - the mouse shows it LED by LED on the shared clock, never with
    // its own effects (they run on the mouse's clock, out of step with the other devices).
    void Set(const fx::Params& effect, double brightness, bool own, bool inStep = false, bool asleep = false);

    // Asleep (device_sleep.h): nothing more goes to the devices until they're used again, so a
    // wireless mouse can sleep.
    void SetAsleep(bool asleep) { std::lock_guard<std::mutex> lock(mutex_); asleep_ = asleep; }
    void SetSystemSuspended(bool suspended) { powerSuspend_.Request(suspended); }
    bool WaitForSuspend(unsigned ms) { return !thread_.joinable() || powerSuspend_.Wait(ms); }

    State state() const { return state_; }
    // Whether the mouse shows the whole effect right now (its own effect or LED by LED),
    // not one color through G HUB.
    bool mouseEffect() const { return mouseEffect_; }
    bool sdkActive() const { return sdkActive_; }
    // Retained during hand-back so a LIGHTSYNC game keeps control without oscillation.
    bool sdkAvailable() const { return sdkAvailable_; }
    // The directly controlled product, so fallbacks remain available to other Logitech gear.
    std::string directName() const;
    std::wstring dllPath() const;

private:
    void Run();

    std::wstring proxyPath_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Off};
    std::atomic<bool> mouseEffect_{false};
    std::atomic<bool> sdkActive_{false};
    std::atomic<bool> sdkAvailable_{false};
    PowerSuspend powerSuspend_;
    std::atomic<bool> inStep_{false};
    mutable std::mutex mutex_;
    fx::Params effect_;
    double brightness_ = 1.0;
    bool own_ = false;
    bool asleep_ = false;  // part of the same snapshot as brightness and ownership
    uint64_t effectSince_ = 0;
    std::wstring dll_;
    std::string directName_;
};

}  // namespace luma::app
