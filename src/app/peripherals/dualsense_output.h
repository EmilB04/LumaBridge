// Sony DualSense lightbar, by USB or Bluetooth (dualsense_protocol.h). Opt-in (experimental,
// untested on real hardware). Own thread; the lightbar is a single color, so there's nothing
// to redraw per-LED the way the Azoth's keys need.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "color.h"
#include "effects.h"

namespace luma::app {

class DualSenseOutput {
public:
    enum class State { Off, NotFound, Active, Released };
    enum class Link { Usb, Bluetooth };

    ~DualSenseOutput() { Stop(); }
    void Start();
    void Stop();
    // `brightness` 0..1 (LumaBridge's brightness slider); `own = false` stops sending (the
    // controller keeps the last color until the game or Steam sets it again).
    void Set(const fx::Params& effect, double brightness, bool own);
    State state() const { return state_; }
    bool bluetooth() const { return link_ == Link::Bluetooth; }
    // Windows' error from the last failed write, if State is NotFound because of one (0: it
    // was simply never found).
    unsigned long lastWriteError() const { return lastWriteError_; }

private:
    void Run();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Off};
    std::atomic<Link> link_{Link::Usb};
    std::atomic<unsigned long> lastWriteError_{0};
    std::mutex mutex_;
    fx::Params effect_;
    double brightness_ = 1.0;
    bool own_ = false;
    uint64_t effectSince_ = 0;
};

}  // namespace luma::app
