// When to take the motherboard's lights back from Armoury Crate. Its lighting service
// re-applies its own lighting when an Aura device comes or goes (an ASUS keyboard unplugged)
// and after sleep, and that takes the controller's channels out of direct mode: LumaBridge's
// frames are then ignored without any error, and the fans stay dark until direct mode is
// entered again. Entering it blanks the LEDs for an instant (a flicker if done all the time),
// so it's only repeated a few times over the seconds after such an event, by when Armoury
// Crate has finished. Only needed while that service runs: the app normally keeps it paused
// (hardware helper). Pure C++, tested.
#pragma once

#include <cstddef>
#include <cstdint>

namespace luma::aurausb {

class DirectModeReclaim {
public:
    // After an event: when (ms) to enter direct mode again.
    static constexpr uint64_t kAfterMs[] = {500, 1500, 3000, 5000, 7500, 10000, 13000};
    static constexpr size_t kSteps = sizeof kAfterMs / sizeof kAfterMs[0];

    // A device came or went, or the PC woke up. True if this starts a schedule (worth a log
    // line); the rest of a burst (one unplug is several device notifications) changes nothing.
    bool OnEvent(uint64_t now) {
        if (Active()) return false;
        eventAt_ = now;
        step_ = 0;
        return true;
    }

    bool Active() const { return step_ < kSteps; }

    // True once each time one (or more) of the moments has passed since the last call.
    bool Due(uint64_t now) {
        bool due = false;
        while (step_ < kSteps && now >= eventAt_ + kAfterMs[step_]) {
            ++step_;
            due = true;
        }
        return due;
    }

private:
    uint64_t eventAt_ = 0;
    size_t step_ = kSteps;  // nothing scheduled
};

}  // namespace luma::aurausb
