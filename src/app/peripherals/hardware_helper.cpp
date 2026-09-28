#include "hardware_helper.h"

#include <array>

#include "helper_ipc.h"
#include "hyperx_ram.h"
#include "log.h"

namespace luma::app {
namespace {

static_assert(helper::kRamLeds == ram::kMaxLeds, "the shared RAM colors cover every LED");

constexpr DWORD kFrameMs = 50;
constexpr wchar_t kTask[] = L"\\LumaBridge\\Hardware helper";

int RunHidden(std::wstring cmdLine, DWORD timeoutMs) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::wstring Schtasks(const wchar_t* verb) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return L"\"" + std::wstring(sys) + L"\\schtasks.exe\" " + verb + L" /tn \"" + kTask + L"\"";
}

const char* RamProblemText(helper::RamStatus s) {
    switch (s) {
    case helper::RamStatus::NoModule: return "The helper's SMBus module is missing: click Set up again.";
    case helper::RamStatus::ModuleFailed:
        return "PawnIO couldn't load its SMBus module. RAM lighting needs an AMD chipset for now.";
    case helper::RamStatus::NotKingston: return "No HyperX / Kingston FURY memory found.";
    case helper::RamStatus::NoController:
        return "No HyperX RGB controller answered on the SMBus (are these the RGB version of the sticks?).";
    case helper::RamStatus::NoSticks: return "No DDR4 sticks answered on the SMBus.";
    case helper::RamStatus::WriteFailed:
        return "Writing the colors kept failing, so LumaBridge stopped (details in %ProgramData%\\LumaBridge\\helper.log).";
    case helper::RamStatus::BusBusy: return "Another program is holding the SMBus.";
    default: return nullptr;
    }
}

}  // namespace

bool HelperTaskInstalled() { return RunHidden(Schtasks(L"/query"), 10000) == 0; }

void HardwareHelper::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    state_ = State::Starting;
    thread_ = std::thread(&HardwareHelper::Run, this);
}

void HardwareHelper::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    state_ = State::Off;
}

void HardwareHelper::SetRam(const fx::Params& effect, double brightness, bool wanted, bool own, int release) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (effect.kind != effect_.kind || effect.speed != effect_.speed) effectSince_ = GetTickCount64();
    effect_ = effect;
    brightness_ = brightness;
    ramWanted_ = wanted;
    ramOwn_ = own;
    ramRelease_ = release;
}

std::string HardwareHelper::ramProblem() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ramProblem_;
}

std::string HardwareHelper::chip() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chip_;
}

std::vector<sensors::Sensor> HardwareHelper::Sensors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sensors_;
}

void HardwareHelper::Run() {
    HANDLE mapping = nullptr;
    helper::Shared* sh = nullptr;
    uint64_t nextStart = 0, connectedAt = 0, helperBeat = 0, helperBeatAt = 0, appBeat = 0;
    uint32_t seq = 0, lastSensorSeq = 0;
    std::array<Rgb, ram::kMaxLeds> last{};
    bool haveLast = false;
    auto disconnect = [&] {
        if (sh) {
            sh->ramOwn = 0;
            UnmapViewOfFile(sh);
        }
        if (mapping) CloseHandle(mapping);
        sh = nullptr;
        mapping = nullptr;
        haveLast = false;
        std::lock_guard<std::mutex> lock(mutex_);
        sensors_.clear();
    };
    while (!stop_) {
        Sleep(kFrameMs);
        fx::Params effect;
        double brightness;
        bool wanted, own;
        int release;
        uint64_t since;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            effect = effect_;
            brightness = brightness_;
            wanted = ramWanted_;
            own = ramOwn_;
            release = ramRelease_;
            since = effectSince_;
        }
        const uint64_t now = GetTickCount64();
        if (!wanted) ramState_ = RamState::Off;

        if (!sh) {
            if (retry_.exchange(false)) nextStart = 0;
            mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, helper::kSharedName);
            if (mapping) {
                sh = static_cast<helper::Shared*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(helper::Shared)));
                if (!sh || sh->magic != helper::kMagic || sh->version != helper::kVersion) {
                    disconnect();  // not ready yet, or a helper from another version
                    continue;
                }
                sh->appBeat = ++appBeat;
                sh->appPid = GetCurrentProcessId();
                connectedAt = helperBeatAt = now;
                helperBeat = sh->helperBeat;
                LUMA_INFO("hardware helper: connected (pid %u)", sh->helperPid);
                continue;
            }
            if (now >= nextStart) {
                nextStart = now + 60000;
                if (RunHidden(Schtasks(L"/run"), 10000) != 0) {
                    if (state_ != State::NotSetUp) LUMA_INFO("hardware helper: not set up (Devices > Hardware access)");
                    state_ = State::NotSetUp;
                } else {
                    state_ = State::Starting;
                    nextStart = now + 20000;
                }
            }
            continue;
        }

        // Connected.
        // Heartbeats by change (the two processes' clocks aren't compared).
        sh->appBeat = ++appBeat;
        if (sh->helperBeat != helperBeat) {
            helperBeat = sh->helperBeat;
            helperBeatAt = now;
        }
        const bool helperGone = helperBeat ? now - helperBeatAt > 5000 : now - connectedAt > 20000;
        if (helperGone) {
            LUMA_INFO("hardware helper: stopped");
            disconnect();
            nextStart = now + 3000;
            state_ = State::Starting;
            continue;
        }
        if (sh->status == helper::Status::NoPawnIO) {
            state_ = State::NoPawnIO;
            continue;
        }
        state_ = sh->status == helper::Status::Running ? State::Running : State::Starting;

        // Sensors (seqlock: copy only a stable, even sequence).
        const uint32_t s1 = sh->sensorSeq;
        if (!(s1 & 1) && s1 != lastSensorSeq) {
            MemoryBarrier();
            std::vector<sensors::Sensor> list;
            const uint32_t n = std::min<uint32_t>(sh->sensorCount, helper::kMaxSensors);
            for (uint32_t i = 0; i < n; ++i) {
                const helper::SensorEntry& e = sh->sensors[i];
                sensors::Sensor s;
                s.kind = e.kind == helper::SensorKind::Cpu ? sensors::HardwareKind::Cpu : sensors::HardwareKind::Board;
                s.type = e.type == helper::SensorType::Fan     ? sensors::SensorType::Fan
                         : e.type == helper::SensorType::Power ? sensors::SensorType::Power
                                                               : sensors::SensorType::Temperature;
                s.value = e.value;
                char name[sizeof e.name + 1] = {};
                memcpy(name, e.name, sizeof e.name);
                s.name = name;
                s.unit = s.type == sensors::SensorType::Fan ? "RPM" : s.type == sensors::SensorType::Power ? "W" : "\xC2\xB0" "C";
                s.hardware = s.kind == sensors::HardwareKind::Cpu ? "CPU" : std::string("Nuvoton ") + sh->chip;
                list.push_back(s);
            }
            MemoryBarrier();
            if (sh->sensorSeq == s1) {
                lastSensorSeq = s1;
                std::lock_guard<std::mutex> lock(mutex_);
                sensors_ = std::move(list);
                char chip[sizeof sh->chip + 1] = {};
                memcpy(chip, sh->chip, sizeof sh->chip);
                chip_ = chip;
            }
        }

        // RAM.
        sh->ramRelease = static_cast<uint32_t>(release);
        sh->ramWanted = wanted ? 1 : 0;
        sh->ramOwn = wanted && own ? 1 : 0;
        if (!wanted) continue;
        const helper::RamStatus ramStatus = sh->ram;
        if (const char* text = RamProblemText(ramStatus)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ramProblem_ != text) LUMA_WARN("RAM lighting: %s", text);
                ramProblem_ = text;
            }
            ramState_ = RamState::Problem;
            continue;
        }
        if (!own) {
            ramState_ = RamState::Released;
            haveLast = false;
            continue;
        }
        std::array<Rgb, ram::kMaxLeds> colors{};
        const double t = fx::Seconds(effect, now, since);
        for (int led = 0; led < ram::kLedsPerStick; ++led) {
            const Rgb c = Scale(fx::Render(effect, t, led, ram::kLedsPerStick), brightness);
            for (int slot = 0; slot < ram::kSlots; ++slot) colors[static_cast<size_t>(slot * ram::kLedsPerStick + led)] = c;
        }
        if (!haveLast || colors != last) {
            for (int i = 0; i < ram::kMaxLeds; ++i) {
                sh->ramColors[i][0] = colors[static_cast<size_t>(i)].r;
                sh->ramColors[i][1] = colors[static_cast<size_t>(i)].g;
                sh->ramColors[i][2] = colors[static_cast<size_t>(i)].b;
            }
            MemoryBarrier();
            sh->ramSeq = ++seq;
            last = colors;
            haveLast = true;
        }
        int n = 0;
        for (int i = 0; i < ram::kSlots; ++i) n += (sh->sticks >> i) & 1;
        sticks_ = n;
        ramState_ = ramStatus == helper::RamStatus::Active ? RamState::Active : RamState::Starting;
    }
    disconnect();
}

}  // namespace luma::app
