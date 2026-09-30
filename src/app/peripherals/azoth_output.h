// ASUS ROG Azoth by cable or through its ROG Omni receiver (see azoth_protocol.h): every key
// its own color (the per-key command, keys in azoth_layout.h). Never Armoury Crate's save
// command, so nothing is written to the keyboard's flash. Opt-in (experimental). Own thread;
// only the keys that changed, at most ~25 updates per second by cable and ~10 wirelessly.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "azoth_protocol.h"
#include "effects.h"

namespace luma::app {

class AzothOutput {
public:
    enum class State { Off, NotFound, Active, Released };

    ~AzothOutput() { Stop(); }
    void Start();
    void Stop();
    // `brightness` 0..1 (LumaBridge's brightness slider); `own = false` stops sending (the
    // keyboard keeps the last color until it restarts; its saved lighting is untouched).
    void Set(const fx::Params& effect, double brightness, bool own);
    // Asleep (device_sleep.h): nothing more goes to the keyboard until it's used again.
    void SetAsleep(bool asleep) { asleep_ = asleep; }
    State state() const { return state_; }
    // How it was last found (meaningful while Active).
    bool wireless() const { return link_ == azoth::Link::Wireless; }
    // Windows' error from the last failed write, if State is NotFound because of one (0: it
    // was simply never found, the more common case).
    unsigned long lastWriteError() const { return lastWriteError_; }

private:
    void Run();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Off};
    std::atomic<bool> asleep_{false};
    std::atomic<azoth::Link> link_{azoth::Link::Wired};
    std::atomic<unsigned long> lastWriteError_{0};
    std::mutex mutex_;
    fx::Params effect_;
    double brightness_ = 1.0;
    bool own_ = false;
    uint64_t effectSince_ = 0;
};

}  // namespace luma::app
