// Logitech devices (G502 X Plus, ...) through Logitech's own LED SDK: LumaBridge loads
// G HUB's SDK DLL in its own process and sets the color; G HUB does the device I/O and
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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "effects.h"

namespace luma::app {

// A Logitech device found over HID++ (through its receiver or its cable).
struct LogitechDevice {
    std::string name;   // as the device reports it, e.g. "G502 X PLUS"
    int type = -1;      // HID++ device type (hidpp::DeviceTypeName), -1 if unknown
    bool rgb = false;   // has RGB lighting (hidpp::kLightingFeatures)
};

// Every Logitech device reachable over HID++, with or without lighting. Talks to each
// receiver and cable, so it takes a few seconds: run it in the background.
std::vector<LogitechDevice> ScanLogitechDevices();

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
    void Set(const fx::Params& effect, double brightness, bool own);

    // Asleep (device_sleep.h): nothing more goes to the devices until they're used again, so a
    // wireless mouse can sleep.
    void SetAsleep(bool asleep) { asleep_ = asleep; }

    State state() const { return state_; }
    // Whether the mouse shows the whole effect right now (its own effect or LED by LED),
    // not one color through G HUB.
    bool mouseEffect() const { return mouseEffect_; }
    std::wstring dllPath() const;

private:
    void Run();

    std::wstring proxyPath_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Off};
    std::atomic<bool> mouseEffect_{false};
    std::atomic<bool> asleep_{false};
    mutable std::mutex mutex_;
    fx::Params effect_;
    double brightness_ = 1.0;
    bool own_ = false;
    uint64_t effectSince_ = 0;
    std::wstring dll_;
};

}  // namespace luma::app
