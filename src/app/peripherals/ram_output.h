// HyperX / Kingston FURY RGB DDR4 lighting (see hyperx_ram.h). The SMBus writes happen in
// the elevated helper LumaBridge-RAM.exe (a scheduled task set up once, see
// scripts\Install-RamTask.ps1); this side starts it, renders LumaBridge's effect across
// each stick's 5 LEDs and hands the colors over through shared memory. Opt-in
// (experimental). Own thread; about 20 updates per second at most.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "effects.h"

namespace luma::app {

// Whether the RAM helper's scheduled task is registered.
bool RamTaskInstalled();

class RamOutput {
public:
    enum class State {
        Off,        // not enabled
        NotSetUp,   // the helper's task isn't registered (Set up needed)
        Starting,   // starting the helper / finding the sticks
        Active,     // following LumaBridge
        Released,   // not driving the RAM right now (Armoury Crate's lighting)
        Problem,    // the helper can't light the RAM: see problem()
    };

    ~RamOutput() { Stop(); }
    void Start();
    void Stop();
    // `brightness` 0..1; `own = false` stops sending (the sticks keep the last color).
    void Set(const fx::Params& effect, double brightness, bool own);
    State state() const { return state_; }
    std::string problem() const;
    int sticks() const { return sticks_; }  // sticks found (while Active)

private:
    void Run();
    void SetProblem(const char* text);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Off};
    std::atomic<int> sticks_{0};
    mutable std::mutex mutex_;
    fx::Params effect_;
    double brightness_ = 1.0;
    bool own_ = false;
    uint64_t effectSince_ = 0;
    std::string problem_;
};

}  // namespace luma::app
