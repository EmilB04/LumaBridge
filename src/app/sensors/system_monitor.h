// Critical system info for the dashboard, gathered on a background thread about once a second
// (and only while someone is looking at it):
//   - names: board, BIOS, CPU, memory modules (SMBIOS), GPUs (DXGI);
//   - CPU load and memory use (Windows);
//   - NVIDIA GPUs: temperature, load, fan, VRAM, power (NVML, shipped with the driver);
//   - fan speeds, CPU / board temperatures: from LumaBridge's hardware helper (through the
//     bundled PawnIO driver, once set up on the Devices page), else from LibreHardwareMonitor's
//     web server when it's running.
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
    double clockMhz = -1, memClockMhz = -1;                   // NVIDIA only
};

struct SystemSnapshot {
    bool ready = false;
    SmbiosInfo smbios;
    std::string cpuName;  // from Windows (cleaner than SMBIOS on some boards)
    int cpuThreads = 0;
    double cpuLoad = -1;  // %
    // The processor's effective clock (MHz) from Windows' performance counters (what Task
    // Manager's "Speed" shows): the rated frequency times the current performance; -1 unknown.
    double cpuClockMhz = -1;
    uint64_t memTotal = 0, memUsed = 0;
    std::vector<GpuStat> gpus;
    bool nvml = false;           // NVIDIA's library is available
    bool lhmConnected = false;   // sensors available (the helper's or LibreHardwareMonitor's)
    std::string sensorSource;    // "LumaBridge (Nuvoton NCT6798D)", "LibreHardwareMonitor"
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
    // Sensors from the hardware helper; preferred over LibreHardwareMonitor's while there are any.
    void SetBuiltInSensors(std::vector<Sensor> sensors, std::string chip);

private:
    void Run();

    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> lastRequest_{0};
    std::thread thread_;
    int lhmPort_ = 8085;
    std::mutex mutex_;
    SystemSnapshot snap_;
    std::vector<Sensor> builtIn_;
    std::string builtInChip_;
};

}  // namespace luma::app::sensors
