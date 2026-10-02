// ASUS ROG Azoth by cable or through its ROG Omni receiver (see azoth_protocol.h): every key
// its own color (the per-key command, keys in azoth_layout.h). RGB never sends Armoury
// Crate's save command. OLED control is separately opt-in (experimental). One thread
// serializes RGB frames and acknowledged display commands, and pauses both while asleep.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "azoth_protocol.h"
#include "azoth_oled.h"
#include "azoth_connection.h"
#include "effects.h"

namespace luma::app {

class AzothOutput {
public:
    enum class State { Off, NotFound, Active, Released };
    enum class OledState { Vendor, Pending, NotFound, Active, Asleep, Failed };

    ~AzothOutput() { Stop(); }
    void Start();
    void Stop();
    // `brightness` 0..1 (LumaBridge's brightness slider); `own = false` stops sending (the
    // keyboard keeps the last color until it restarts; its saved lighting is untouched).
    void Set(const fx::Params& effect, double brightness, bool own, bool asleep = false);
    void SetOled(azoth::OledSettings settings);
    void ReapplyOled();
    OledState oledState() const { return oledState_; }
    unsigned long oledError() const { return oledError_; }
    int oledAnimation() const { return oledAnimation_; }
    std::string oledFailureDetails() const;
    azoth::Connection connection() const { return connection_; }
    void Rescan() { rescan_ = true; }
    // Asleep (device_sleep.h): nothing more goes to the keyboard until it's used again.
    State state() const { return state_; }
    bool wireless() const { return connection_ == azoth::Connection::Wireless; }
    // Windows' error from the last failed write, if State is NotFound because of one (0: it
    // was simply never found, the more common case).
    unsigned long lastWriteError() const { return lastWriteError_; }
    unsigned long sleepTimerError() const { return sleepTimerError_; }

private:
    void Run();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Off};
    std::atomic<azoth::Link> link_{azoth::Link::Wired};
    std::atomic<azoth::Connection> connection_{azoth::Connection::Unknown};
    std::atomic<bool> rescan_{true};
    std::atomic<unsigned long> lastWriteError_{0};
    std::atomic<unsigned long> sleepTimerError_{0};
    std::atomic<OledState> oledState_{OledState::Vendor};
    std::atomic<unsigned long> oledError_{0};
    std::atomic<int> oledAnimation_{-1};
    mutable std::mutex mutex_;
    std::string oledFailureDetails_;
    fx::Params effect_;
    double brightness_ = 1.0;
    bool own_ = false;
    bool asleep_ = false;  // same snapshot as the final RGB brightness
    uint64_t effectSince_ = 0;
    azoth::OledSettings oled_;
    uint64_t oledRevision_ = 0;
};

}  // namespace luma::app
