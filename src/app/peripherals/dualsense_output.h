// Sony DualSense lightbar, by USB or Bluetooth (dualsense_protocol.h). Opt-in; both links
// are confirmed on real hardware. Own thread; the lightbar is a single color, so there's nothing
// to redraw per-LED the way the Azoth's keys need.
#pragma once
#include "delivery_receipt.h"
#include "worker_cancellation.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "color.h"
#include "effects.h"
#include "dualsense_effects.h"

namespace luma::app {

class DualSenseOutput {
public:
    enum class State { Off, NotFound, Active, Released, Connecting };
    enum class Link { Usb, Bluetooth };

    ~DualSenseOutput() { Stop(); }
    void Start();
    void RequestStop() { stop_ = true; cancellation_.Cancel(); }
    uint64_t delivered() const { return delivery_.value(); }
    void Stop();
    // `brightness` 0..1 (LumaBridge's brightness slider); `own = false` stops sending (the
    // controller keeps the last color until the game or Steam sets it again).
    // `playerLeds`: moving effects also show on the five player LEDs (dualsense_effects.h).
    void Set(const fx::Params& effect, double brightness, bool own, bool playerLeds = true, const dualsense::PlayerOptions& options = {});
    State state() const { return state_; }
    bool bluetooth() const { return link_ == Link::Bluetooth; }
    // Windows' error from the last failed write, if State is NotFound because of one (0: it
    // was simply never found).
    unsigned long lastWriteError() const { return lastWriteError_; }

private:
    luma::DeliveryReceipt delivery_;
    void Run();

    WorkerCancellation cancellation_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Off};
    std::atomic<Link> link_{Link::Usb};
    std::atomic<unsigned long> lastWriteError_{0};
    std::mutex mutex_;
    fx::Params effect_;
    double brightness_ = 1.0;
    bool own_ = false;
    bool playerLeds_ = true;
    dualsense::PlayerOptions playerOptions_;
    uint64_t effectSince_ = 0;
};

}  // namespace luma::app
