// NZXT Kraken AIO coolers (X53/X63/X73, Z53/Z63/Z73, Kraken and Kraken Elite 2023+): reading
// their status over USB HID, as documented by liquidctl (the open-source cooler tool): the
// liquid's temperature, the pump's speed and duty, and on the screen models the fans on the
// pump's fan header. Read only: the lighting and the screen stay with NZXT CAM.
// The parsing is pure and tested; the reading is in nzxt_kraken.cpp.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace luma::app::nzxt {

constexpr uint16_t kVid = 0x1E71;

struct Status {
    bool valid = false;
    double liquidC = -1;  // °C
    int pumpRpm = -1, pumpDuty = -1;  // RPM, %
    int fanRpm = -1, fanDuty = -1;    // the fans on the pump (screen models), -1 if not reported
};

// A status report (as read, starting with its report ID 0x75): the X models send them on their
// own (0x75 0x02), the screen models answer a request (0x74 0x01 -> 0x75 0x01).
inline Status Parse(const uint8_t* m, size_t n, bool screenModel) {
    Status s;
    if (n < 20 || m[0] != 0x75 || (m[1] != 0x01 && m[1] != 0x02)) return s;
    if (m[15] == 0xFF && m[16] == 0xFF) return s;  // no liquid reading yet
    s.liquidC = m[15] + m[16] / 10.0;
    s.pumpRpm = m[17] | m[18] << 8;
    s.pumpDuty = m[19];
    if (screenModel && n >= 26) {
        const int rpm = m[23] | m[24] << 8;
        if (rpm > 0 && rpm < 10000) {
            s.fanRpm = rpm;
            s.fanDuty = m[25];
        }
    }
    s.valid = s.liquidC > 0 && s.liquidC < 100 && s.pumpRpm >= 0 && s.pumpRpm < 10000;
    return s;
}

// NZXT Krakens found by their USB name ("NZXT Kraken Elite" ...), whatever their product ID:
// (vendor, product) and the name, one per device.
struct Named {
    uint16_t vid, pid;
    std::string name;
};
std::vector<Named> FindByName();

// Where reading stands, for the UI.
enum class KrakenState { Off, Searching, CantOpen, NoReply, Reading };

// Reads a Kraken's status about once a second, in the background, while one is plugged in.
class Kraken {
public:
    ~Kraken() { Stop(); }
    // `pid`: the Kraken's USB product ID (0: none), `screen`: a screen model.
    void Start(uint16_t pid, bool screen);
    void Stop();
    Status status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }
    bool running() const { return thread_.joinable(); }
    KrakenState state() const { return state_; }
    // Opened to listen only (NZXT CAM has it open and shares it for reading, not writing):
    // LumaBridge then reads the status replies CAM asks for.
    bool listening() const { return listening_; }
    unsigned long lastError() const { return lastError_; }  // Windows' error opening it (0: none)

private:
    void Run(uint16_t pid, bool screen);
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<KrakenState> state_{KrakenState::Off};
    std::atomic<bool> listening_{false};
    std::atomic<unsigned long> lastError_{0};
    mutable std::mutex mutex_;
    Status status_;
};

}  // namespace luma::app::nzxt
