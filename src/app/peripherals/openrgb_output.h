// Every device OpenRGB supports, through OpenRGB's SDK server (openrgb_protocol.h): optional,
// only when OpenRGB runs with its SDK server on. LumaBridge sends each LED's color; when it
// lets go, each device gets back the mode it had (its own effect).
// Runs on its own thread (~30 updates per second, only changed devices are sent).
#pragma once
#include "delivery_receipt.h"
#include "worker_cancellation.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "effects.h"

namespace luma::app {

struct OpenRgbDevice {
    std::string name, vendor;
    int type = -1;  // openrgb::TypeName
    uint32_t leds = 0;
    std::string id, location, serial;
};

class OpenRgbOutput {
public:
    enum class State { Off, NotRunning, Connected };

    ~OpenRgbOutput() { Stop(); }
    void Start(uint16_t port);
    void RequestStop() { stop_ = true; cancellation_.Cancel(); }
    uint64_t delivered() const { return delivery_.value(); }
    void Stop();

    // What the devices should show. `own = false` gives every device its own effect back;
    // `skip`: identities of devices LumaBridge leaves alone (lit natively, or switched off).
    void Set(const fx::Params& effect, double brightness, bool own, const std::vector<std::string>& skip);

    void KeepLastColor(bool keep) { keepLastColor_ = keep; }
    State state() const { return state_; }
    std::vector<OpenRgbDevice> devices() const;

private:
    luma::DeliveryReceipt delivery_;
    void Run();

    uint16_t port_ = 0;
    WorkerCancellation cancellation_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> keepLastColor_{false};
    std::atomic<State> state_{State::Off};
    mutable std::mutex mutex_;
    fx::Params effect_;
    double brightness_ = 1.0;
    bool own_ = false;
    std::vector<std::string> skip_;
    uint64_t effectSince_ = 0;
    std::vector<OpenRgbDevice> devices_;
};

}  // namespace luma::app
