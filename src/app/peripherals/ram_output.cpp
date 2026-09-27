#include "ram_output.h"

#include <array>

#include "hyperx_ram.h"
#include "log.h"

namespace luma::app {
namespace {

constexpr DWORD kFrameMs = 50;
constexpr wchar_t kTask[] = L"\\LumaBridge\\RAM lighting";

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

const char* ProblemText(ram::HelperStatus s) {
    switch (s) {
    case ram::HelperStatus::NoPawnIO: return "The PawnIO driver isn't installed (pawnio.eu).";
    case ram::HelperStatus::NoModule:
        return "SmbusPIIX4.bin is missing: put it next to LumaBridge.exe and click Set up again.";
    case ram::HelperStatus::ModuleFailed:
        return "PawnIO couldn't load its SMBus module. RAM lighting needs an AMD chipset for now.";
    case ram::HelperStatus::NotKingston: return "No HyperX / Kingston FURY memory found.";
    case ram::HelperStatus::NoController:
        return "No HyperX RGB controller answered on the SMBus (are these the RGB version of the sticks?).";
    case ram::HelperStatus::NoSticks: return "No DDR4 sticks answered on the SMBus.";
    case ram::HelperStatus::WriteFailed:
        return "Writing the colors kept failing, so LumaBridge stopped (details in %ProgramData%\\LumaBridge\\ram.log).";
    case ram::HelperStatus::BusBusy: return "Another program is holding the SMBus.";
    default: return nullptr;
    }
}

}  // namespace

bool RamTaskInstalled() { return RunHidden(Schtasks(L"/query"), 10000) == 0; }

void RamOutput::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    state_ = State::Starting;
    thread_ = std::thread(&RamOutput::Run, this);
}

void RamOutput::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    state_ = State::Off;
}

void RamOutput::Set(const fx::Params& effect, double brightness, bool own) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (effect.kind != effect_.kind || effect.speed != effect_.speed) effectSince_ = GetTickCount64();
    effect_ = effect;
    brightness_ = brightness;
    own_ = own;
}

std::string RamOutput::problem() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return problem_;
}

void RamOutput::SetProblem(const char* text) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (problem_ != text) LUMA_WARN("RAM lighting: %s", text);
        problem_ = text;
    }
    state_ = State::Problem;
}

void RamOutput::Run() {
    HANDLE mapping = nullptr;
    ram::Shared* sh = nullptr;
    uint64_t nextStart = 0, connectedAt = 0;
    uint32_t seq = 0;
    std::array<Rgb, ram::kMaxLeds> last{};
    bool haveLast = false;
    auto disconnect = [&] {
        if (sh) {
            sh->own = 0;
            UnmapViewOfFile(sh);
        }
        if (mapping) CloseHandle(mapping);
        sh = nullptr;
        mapping = nullptr;
        haveLast = false;
    };
    while (!stop_) {
        Sleep(kFrameMs);
        fx::Params effect;
        double brightness;
        bool own;
        uint64_t since;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            effect = effect_;
            brightness = brightness_;
            own = own_;
            since = effectSince_;
        }
        const uint64_t now = GetTickCount64();

        if (!sh) {
            if (!own) {
                if (state_ != State::Problem && state_ != State::NotSetUp) state_ = State::Released;
                continue;
            }
            mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, ram::kSharedName);
            if (mapping) {
                sh = static_cast<ram::Shared*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ram::Shared)));
                if (!sh || sh->magic != ram::kMagic || sh->version != ram::kVersion) {
                    disconnect();  // not ready yet, or a helper from another version
                    continue;
                }
                sh->appBeat = now;
                sh->appPid = GetCurrentProcessId();
                connectedAt = now;
                LUMA_INFO("RAM lighting: connected to the helper (pid %u)", sh->helperPid);
                continue;
            }
            if (now >= nextStart) {
                nextStart = now + 20000;
                if (RunHidden(Schtasks(L"/run"), 10000) != 0) {
                    state_ = State::NotSetUp;
                    LUMA_INFO("RAM lighting: the helper's task isn't set up");
                } else if (state_ != State::Problem) {
                    state_ = State::Starting;
                }
            }
            continue;
        }

        // Connected.
        sh->appBeat = now;
        const bool helperGone = sh->helperBeat ? now - sh->helperBeat > 5000 : now - connectedAt > 20000;
        if (helperGone) {
            LUMA_INFO("RAM lighting: the helper stopped");
            disconnect();
            nextStart = now + (state_ == State::Problem ? 60000 : 3000);
            continue;
        }
        const ram::HelperStatus status = sh->status;
        if (const char* text = ProblemText(status)) {
            SetProblem(text);
            continue;
        }
        sh->own = own ? 1 : 0;
        if (!own) {
            state_ = State::Released;
            haveLast = false;
            continue;
        }
        std::array<Rgb, ram::kMaxLeds> colors{};
        const double t = static_cast<double>(now - since) / 1000.0;
        for (int led = 0; led < ram::kLedsPerStick; ++led) {
            const Rgb c = Scale(fx::Render(effect, t, led, ram::kLedsPerStick), brightness);
            for (int slot = 0; slot < ram::kSlots; ++slot) colors[static_cast<size_t>(slot * ram::kLedsPerStick + led)] = c;
        }
        if (!haveLast || colors != last) {
            for (int i = 0; i < ram::kMaxLeds; ++i) {
                sh->colors[i][0] = colors[static_cast<size_t>(i)].r;
                sh->colors[i][1] = colors[static_cast<size_t>(i)].g;
                sh->colors[i][2] = colors[static_cast<size_t>(i)].b;
            }
            MemoryBarrier();
            sh->seq = ++seq;
            last = colors;
            haveLast = true;
        }
        int n = 0;
        for (int i = 0; i < ram::kSlots; ++i) n += (sh->sticks >> i) & 1;
        sticks_ = n;
        state_ = status == ram::HelperStatus::Active ? State::Active : State::Starting;
    }
    disconnect();
}

}  // namespace luma::app
