// Critical system info for the dashboard, gathered on a background thread about once a second
// (and only while someone is looking at it):
//   - names: board, BIOS, CPU, memory modules (SMBIOS), GPUs (DXGI);
//   - CPU load and memory use (Windows);
//   - NVIDIA GPUs: temperature, load, fan, VRAM, power (NVML, shipped with the driver);
//   - everything else - fan speeds, CPU / board temperatures - from LibreHardwareMonitor's
//     web server when it's running (reading those sensors needs a kernel driver, which
//     LumaBridge deliberately doesn't install).
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lhm.h"
#include "smbios.h"

namespace luma::app::sensors {

struct GpuStat {
    std::string name;
    uint64_t vramTotal = 0, vramUsed = 0;  // bytes (used: NVIDIA only)
    double temp = -1, load = -1, fanPct = -1, powerW = -1;  // -1 = unknown
};

struct SystemSnapshot {
    bool ready = false;
    SmbiosInfo smbios;
    std::string cpuName;  // from Windows (cleaner than SMBIOS on some boards)
    int cpuThreads = 0;
    double cpuLoad = -1;  // %
    uint64_t memTotal = 0, memUsed = 0;
    std::vector<GpuStat> gpus;
    bool nvml = false;           // NVIDIA's library is available
    bool lhmConnected = false;   // LibreHardwareMonitor answered
    std::vector<Sensor> lhm;
    uint64_t updatedAt = 0;
};

class SystemMonitor {
public:
    ~SystemMonitor() { Stop(); }
    void Start(int lhmPort);
    void Stop();
    // The latest data; also keeps the poller awake for a few seconds.
    SystemSnapshot Snapshot();
    int lhmPort() const { return lhmPort_; }

private:
    void Run();

    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> lastRequest_{0};
    std::thread thread_;
    int lhmPort_ = 8085;
    std::mutex mutex_;
    SystemSnapshot snap_;
};

}  // namespace luma::app::sensors
