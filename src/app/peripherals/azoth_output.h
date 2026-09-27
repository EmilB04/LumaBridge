// ASUS ROG Azoth by cable or through its ROG Omni receiver (see azoth_protocol.h): LumaBridge's color on
// the whole keyboard, using the exact static-color command Armoury Crate sends - and never
// its save command, so nothing is written to the keyboard's flash. Opt-in (experimental).
// Own thread; at most ~10 updates per second.
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
    State state() const { return state_; }
    // How it was last found (meaningful while Active).
    bool wireless() const { return link_ == azoth::Link::Wireless; }

private:
    void Run();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Off};
    std::atomic<azoth::Link> link_{azoth::Link::Wired};
    std::mutex mutex_;
    fx::Params effect_;
    double brightness_ = 1.0;
    bool own_ = false;
    uint64_t effectSince_ = 0;
};

}  // namespace luma::app
