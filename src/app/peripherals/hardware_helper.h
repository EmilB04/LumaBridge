// The app's side of the hardware helper (LumaBridge-Helper.exe, see src/helper): starts it
// whenever LumaBridge runs (once it's set up - Devices > Hardware access), hands it the RAM
// colors, and collects the sensors it reads (fans, CPU / board temperatures) for the
// dashboard. Own thread; about 20 RAM updates per second at most.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "effects.h"
#include "lhm.h"

namespace luma::app {

// Whether the helper's scheduled task is registered.
bool HelperTaskInstalled();

class HardwareHelper {
public:
    enum class State {
        Off,       // not started
        NotSetUp,  // the helper's task isn't registered (Set up needed)
        Starting,  // starting the helper
        Running,   // connected
        NoPawnIO,  // the helper runs, but the PawnIO driver isn't installed (Set up again)
    };
    enum class RamState {
        Off,       // RAM lighting not switched on
        Starting,  // finding the sticks
        Active,    // following LumaBridge
        Released,  // not driving the RAM right now (Armoury Crate's lighting)
        Problem,   // can't light the RAM: see ramProblem()
    };

    ~HardwareHelper() { Stop(); }
    void Start();
    void Stop();
    // Try starting the helper again now (e.g. right after it was set up).
    void Retry() { retry_ = true; }
    // `brightness` 0..1; `wanted`: RAM lighting switched on; `own = false` lets go of the
    // sticks, which then show `release` (helper::RamRelease: their own rainbow, off, last color).
    void SetRam(const fx::Params& effect, double brightness, bool wanted, bool own, int release);

    State state() const { return state_; }
    RamState ramState() const { return ramState_; }
    std::string ramProblem() const;
    int sticks() const { return sticks_; }  // RAM sticks found
    std::string chip() const;               // the board's monitoring chip, "" if none
    // The latest sensors (empty when the helper isn't running).
    std::vector<sensors::Sensor> Sensors() const;

private:
    void Run();

    std::thread thread_;
    std::atomic<bool> stop_{false}, retry_{false};
    std::atomic<State> state_{State::Off};
    std::atomic<RamState> ramState_{RamState::Off};
    std::atomic<int> sticks_{0};
    mutable std::mutex mutex_;
    fx::Params effect_;
    double brightness_ = 1.0;
    bool ramWanted_ = false, ramOwn_ = false;
    int ramRelease_ = 0;
    uint64_t effectSince_ = 0;
    std::string ramProblem_, chip_;
    std::vector<sensors::Sensor> sensors_;
};

}  // namespace luma::app
