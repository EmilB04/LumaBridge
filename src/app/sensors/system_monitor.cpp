#include "system_monitor.h"

#include <dxgi.h>
#include <pdh.h>
#include <winhttp.h>

#include <cstring>

#include "json.h"
#include "log.h"

namespace luma::app::sensors {
namespace {

constexpr uint64_t kIdleAfterMs = 4000;  // stop polling this long after the last Snapshot()

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

constexpr DWORD kRsmb = 0x52534D42;  // 'RSMB': the raw SMBIOS table provider

SmbiosInfo ReadSmbios() {
    const UINT size = GetSystemFirmwareTable(kRsmb, 0, nullptr, 0);
    if (!size) return {};
    std::vector<uint8_t> raw(size);
    if (GetSystemFirmwareTable(kRsmb, 0, raw.data(), size) != size) return {};
    return ParseSmbios(raw);
}

std::string CpuNameFromRegistry() {
    wchar_t buf[256];
    DWORD bytes = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS)
        return {};
    std::string s = Narrow(buf);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::vector<GpuStat> DxgiGpus() {
    std::vector<GpuStat> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return out;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            GpuStat g;
            g.name = Narrow(d.Description);
            g.vramTotal = d.DedicatedVideoMemory;
            bool dup = false;
            for (const auto& x : out) dup |= x.name == g.name;
            if (!dup) out.push_back(g);
        }
        a->Release();
    }
    factory->Release();
    return out;
}

// ---- NVML (nvml.dll ships with the NVIDIA driver) -----------------------------------------

struct Nvml {
    using Fn0 = int(__cdecl*)();
    using GetCount = int(__cdecl*)(unsigned*);
    using GetHandle = int(__cdecl*)(unsigned, void**);
    using GetName = int(__cdecl*)(void*, char*, unsigned);
    using GetTemp = int(__cdecl*)(void*, int, unsigned*);
    using GetUtil = int(__cdecl*)(void*, unsigned*);  // nvmlUtilization_t {gpu, memory}
    using GetUInt = int(__cdecl*)(void*, unsigned*);
    using GetMem = int(__cdecl*)(void*, unsigned long long*);  // nvmlMemory_t {total, free, used}
    using GetClock = int(__cdecl*)(void*, int, unsigned*);     // nvmlClockType_t: 0 graphics, 2 memory

    HMODULE lib = nullptr;
    Fn0 init = nullptr, shutdown = nullptr;
    GetCount count = nullptr;
    GetHandle handle = nullptr;
    GetName name = nullptr;
    GetTemp temp = nullptr;
    GetUtil util = nullptr;
    GetUInt fan = nullptr, power = nullptr;
    GetMem mem = nullptr;
    GetClock clock = nullptr;
    bool ok = false;

    bool Load() {
        lib = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib) {
            wchar_t pf[MAX_PATH];
            if (GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH))
                lib = LoadLibraryW((std::wstring(pf) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll").c_str());
        }
        if (!lib) return false;
        // Through void(*)(): the one function type any function pointer may be cast via.
        auto get = [&](const char* n) { return reinterpret_cast<void (*)()>(GetProcAddress(lib, n)); };
        init = reinterpret_cast<Fn0>(get("nvmlInit_v2"));
        shutdown = reinterpret_cast<Fn0>(get("nvmlShutdown"));
        count = reinterpret_cast<GetCount>(get("nvmlDeviceGetCount_v2"));
        handle = reinterpret_cast<GetHandle>(get("nvmlDeviceGetHandleByIndex_v2"));
        name = reinterpret_cast<GetName>(get("nvmlDeviceGetName"));
        temp = reinterpret_cast<GetTemp>(get("nvmlDeviceGetTemperature"));
        util = reinterpret_cast<GetUtil>(get("nvmlDeviceGetUtilizationRates"));
        fan = reinterpret_cast<GetUInt>(get("nvmlDeviceGetFanSpeed"));
        power = reinterpret_cast<GetUInt>(get("nvmlDeviceGetPowerUsage"));
        mem = reinterpret_cast<GetMem>(get("nvmlDeviceGetMemoryInfo"));
        clock = reinterpret_cast<GetClock>(get("nvmlDeviceGetClockInfo"));
        ok = init && shutdown && count && handle && name && init() == 0;
        if (!ok) {
            FreeLibrary(lib);
            lib = nullptr;
        }
        return ok;
    }
    void Unload() {
        if (ok) shutdown();
        if (lib) FreeLibrary(lib);
        lib = nullptr;
        ok = false;
    }
    // Fills in stats for NVIDIA GPUs (matched to DXGI entries by name, else appended).
    void Read(std::vector<GpuStat>* gpus) const {
        unsigned n = 0;
        if (!ok || count(&n) != 0) return;
        for (unsigned i = 0; i < n; ++i) {
            void* dev = nullptr;
            if (handle(i, &dev) != 0) continue;
            char nm[96] = {};
            name(dev, nm, sizeof nm);
            GpuStat* g = nullptr;
            for (auto& x : *gpus)
                if (x.name == nm || x.name.find(nm) != std::string::npos) g = &x;
            if (!g) {
                gpus->push_back(GpuStat{});
                g = &gpus->back();
                g->name = nm;
            }
            unsigned v = 0, u[2] = {};
            unsigned long long m[3] = {};
            if (temp && temp(dev, 0, &v) == 0) g->temp = v;
            if (util && util(dev, u) == 0) g->load = u[0];
            if (fan && fan(dev, &v) == 0) g->fanPct = v;
            if (power && power(dev, &v) == 0) g->powerW = v / 1000.0;
            if (clock && clock(dev, 0, &v) == 0) g->clockMhz = v;
            if (clock && clock(dev, 2, &v) == 0) g->memClockMhz = v;
            if (mem && mem(dev, m) == 0) {
                g->vramTotal = m[0];
                g->vramUsed = m[2];
            }
        }
    }
};

// ---- The processor's clock (Windows' performance counters) ---------------------------------
// "Processor Frequency" is the rated clock; "% Processor Performance" how far above or below
// it the cores run right now (above 100 while boosting). English names, so it works whatever
// language Windows is in.

struct CpuClock {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER freq = nullptr, perf = nullptr;
    bool ok = false;

    bool Open() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return false;
        ok = PdhAddEnglishCounterW(query, L"\\Processor Information(_Total)\\Processor Frequency", 0, &freq) == ERROR_SUCCESS &&
             PdhAddEnglishCounterW(query, L"\\Processor Information(_Total)\\% Processor Performance", 0, &perf) == ERROR_SUCCESS &&
             PdhCollectQueryData(query) == ERROR_SUCCESS;
        if (!ok) Close();
        return ok;
    }
    void Close() {
        if (query) PdhCloseQuery(query);
        query = nullptr;
        ok = false;
    }
    // MHz, or -1 (needs two samples: the first call only primes the counters).
    double Read() {
        if (!ok || PdhCollectQueryData(query) != ERROR_SUCCESS) return -1;
        PDH_FMT_COUNTERVALUE f{}, p{};
        if (PdhGetFormattedCounterValue(freq, PDH_FMT_DOUBLE, nullptr, &f) != ERROR_SUCCESS ||
            PdhGetFormattedCounterValue(perf, PDH_FMT_DOUBLE, nullptr, &p) != ERROR_SUCCESS)
            return -1;
        const double mhz = f.doubleValue * p.doubleValue / 100.0;
        return mhz > 100 && mhz < 10000 ? mhz : -1;
    }
};

// ---- LibreHardwareMonitor --------------------------------------------------------------

bool ReadLhm(HINTERNET session, int port, std::vector<Sensor>* out) {
    bool ok = false;
    HINTERNET con = WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", L"/data.json", nullptr, nullptr, nullptr, 0) : nullptr;
    if (req && WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        std::string body;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < (8u << 20)) {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got) break;
            body.append(chunk.data(), got);
        }
        Json j;
        if (Json::Parse(body, &j)) {
            *out = ParseLhm(j);
            ok = true;
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    return ok;
}

uint64_t FileTimeValue(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

}  // namespace

void SystemMonitor::SetBuiltInSensors(std::vector<Sensor> sensors, std::string chip) {
    std::lock_guard<std::mutex> lock(mutex_);
    builtIn_ = std::move(sensors);
    builtInChip_ = std::move(chip);
}

void SystemMonitor::Start(int lhmPort) {
    if (thread_.joinable()) return;
    lhmPort_ = lhmPort > 0 && lhmPort < 65536 ? lhmPort : 8085;
    stop_ = false;
    thread_ = std::thread(&SystemMonitor::Run, this);
}

void SystemMonitor::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
}

SystemSnapshot SystemMonitor::Snapshot() {
    lastRequest_ = GetTickCount64();
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

void SystemMonitor::Run() {
    // Names first (once): cheap, and the dashboard can show them straight away.
    {
        SystemSnapshot s;
        s.smbios = ReadSmbios();
        s.cpuName = CpuNameFromRegistry();
        if (s.cpuName.empty()) s.cpuName = s.smbios.cpu;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        s.cpuThreads = static_cast<int>(si.dwNumberOfProcessors);
        s.gpus = DxgiGpus();
        std::lock_guard<std::mutex> lock(mutex_);
        snap_ = s;
    }
    Nvml nvml;
    const bool haveNvml = nvml.Load();
    HINTERNET http = WinHttpOpen(L"LumaBridge", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (http) WinHttpSetTimeouts(http, 300, 300, 500, 800);

    FILETIME idle0{}, kernel0{}, user0{};
    GetSystemTimes(&idle0, &kernel0, &user0);
    CpuClock cpuClock;
    if (!cpuClock.Open()) LUMA_INFO("dashboard: Windows' processor clock counters aren't available");
    uint64_t nextLhm = 0;
    bool lhmOk = false;
    std::vector<Sensor> lhm;
    while (!stop_) {
        Sleep(250);
        const uint64_t now = GetTickCount64();
        if (now - lastRequest_ > kIdleAfterMs) continue;  // nobody is looking
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (snap_.updatedAt && now - snap_.updatedAt < 1000) continue;
        }
        SystemSnapshot s;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            s = snap_;
        }
        // CPU load from the change in idle vs. busy time.
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user)) {
            const uint64_t di = FileTimeValue(idle) - FileTimeValue(idle0);
            const uint64_t dt = (FileTimeValue(kernel) - FileTimeValue(kernel0)) + (FileTimeValue(user) - FileTimeValue(user0));
            if (dt) s.cpuLoad = 100.0 * (1.0 - static_cast<double>(di) / static_cast<double>(dt));
            idle0 = idle;
            kernel0 = kernel;
            user0 = user;
        }
        s.cpuClockMhz = cpuClock.Read();
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) {
            s.memTotal = ms.ullTotalPhys;
            s.memUsed = ms.ullTotalPhys - ms.ullAvailPhys;
        }
        s.nvml = haveNvml;
        if (haveNvml) nvml.Read(&s.gpus);
        std::vector<Sensor> builtIn;
        std::string chip;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            builtIn = builtIn_;
            chip = builtInChip_;
        }
        // LibreHardwareMonitor (only while the helper has no sensors): every second while it
        // answers, every 10 s while it doesn't.
        if (http && builtIn.empty() && now >= nextLhm) {
            const bool was = lhmOk;
            lhmOk = ReadLhm(http, lhmPort_, &lhm);
            if (!lhmOk) lhm.clear();
            if (lhmOk != was)
                LUMA_INFO("dashboard: LibreHardwareMonitor %s on port %d", lhmOk ? "connected" : "not answering", lhmPort_);
            nextLhm = now + (lhmOk ? 1000 : 10000);
        }
        if (!builtIn.empty()) {
            s.lhmConnected = true;
            s.lhm = std::move(builtIn);
            s.sensorSource = chip.empty() ? "LumaBridge" : "LumaBridge (Nuvoton " + chip + ")";
        } else {
            s.lhmConnected = lhmOk;
            s.lhm = lhmOk ? lhm : std::vector<Sensor>();
            s.sensorSource = lhmOk ? "LibreHardwareMonitor" : "";
        }
        s.ready = true;
        s.updatedAt = now;
        std::lock_guard<std::mutex> lock(mutex_);
        snap_ = std::move(s);
    }
    if (http) WinHttpCloseHandle(http);
    cpuClock.Close();
    nvml.Unload();
}

}  // namespace luma::app::sensors
