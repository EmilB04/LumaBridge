// Devices with Windows' lighting standard built in (HID LampArray, lamparray.h): keyboards,
// mice, headsets, cases, light strips of any brand that implement it, lit directly, with no
// vendor software. When LumaBridge lets go, each device runs its own effect again.
// Runs on its own thread; looks for new devices every 15 seconds.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "effects.h"

namespace luma::app {

struct LampArrayDevice {
    std::string name;      // the device's product string ("BlackWidow V4", ...)
    uint16_t vid = 0, pid = 0;
    uint32_t kind = 0;     // lamparray::KindName
    uint32_t lamps = 0;
    std::string problem;   // why it can't be lit ("" when it can)
};

class LampArrayOutput {
public:
    enum class State { Off, Running };

    ~LampArrayOutput() { Stop(); }
    void Start();
    void Stop();

    // What the devices should show. `own = false` gives every device its own effect back;
    // `skip`: names of devices LumaBridge leaves alone (lit natively, or switched off).
    void Set(const fx::Params& effect, double brightness, bool own, const std::vector<std::string>& skip);

    State state() const { return state_; }
    std::vector<LampArrayDevice> devices() const;

private:
    void Run();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Off};
    mutable std::mutex mutex_;
    fx::Params effect_;
    double brightness_ = 1.0;
    bool own_ = false;
    std::vector<std::string> skip_;
    uint64_t effectSince_ = 0;
    std::vector<LampArrayDevice> devices_;
};

}  // namespace luma::app
