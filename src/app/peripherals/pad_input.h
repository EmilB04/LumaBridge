// Reads a connected Sony DualSense or DualShock 4 (USB or Bluetooth): live buttons, sticks,
// triggers, touchpad and motion, battery, and how steadily it reports. It also sends mapped keys
// and mouse movement (pad_mapping.h) when a mapping is on. Own thread; the controller's lighting
// (dualsense_output.h) is separate and can run at the same time.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "pad_mapping.h"
#include "pad_protocol.h"

namespace luma::app {

class PadInput {
public:
    struct Snapshot {
        bool connected = false;
        bool bluetooth = false;
        pad::State state;
        double rateHz = 0, averageMs = 0, maxMs = 0, jitterMs = 0;
        std::vector<float> history;  // report intervals (ms), oldest first
        uint64_t reports = 0;
        bool mapping = false;        // keys are being sent
        unsigned long error = 0;     // Windows' error from the last failed read
    };

    ~PadInput() { Stop(); }
    void Start();
    void Stop();
    Snapshot Get() const;
    // The mapping to apply (copied). `mapping.enabled` off releases every key it holds.
    void SetMapping(const pad::Mapping& mapping);

private:
    void Run();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    Snapshot snap_;
    pad::ReportStats stats_;
    pad::Mapping mapping_;
};

}  // namespace luma::app
