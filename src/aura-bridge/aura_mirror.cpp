#include "aura_mirror.h"

#include <objbase.h>

#include <algorithm>
#include <cwctype>

#include "ipc.h"
#include "log.h"

namespace luma {
namespace {

constexpr uint64_t kReconnectDelayMs = 5000;
constexpr DWORD kIdleWaitMs = 1000;
constexpr uint64_t kAppKeepAliveMs = 1000;  // resend the current color so the app knows we're alive
constexpr UINT kSendTimeoutMs = 200;

bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
    return it != hay.end();
}

// Waits on `h` while pumping messages: a single-threaded COM apartment must keep its
// message queue moving or COM calls into it (and its own outgoing calls) can stall.
DWORD PumpingWait(HANDLE h, DWORD ms) {
    const uint64_t deadline = GetTickCount64() + ms;
    for (;;) {
        const uint64_t now = GetTickCount64();
        const DWORD left = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        DWORD r = MsgWaitForMultipleObjectsEx(1, &h, left, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r != WAIT_OBJECT_0 + 1) return r;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (left == 0) return WAIT_TIMEOUT;
    }
}

HWND FindApp() { return FindWindowExW(HWND_MESSAGE, nullptr, ipc::kAppWindowClass, nullptr); }

bool SendToApp(HWND app, const ipc::Frame& f) {
    COPYDATASTRUCT cds{};
    cds.dwData = ipc::kCopyDataTag;
    cds.cbData = sizeof f;
    cds.lpData = const_cast<ipc::Frame*>(&f);
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(app, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                               SMTO_ABORTIFHUNG | SMTO_BLOCK, kSendTimeoutMs, &result) != 0;
}

}  // namespace

bool AuraMirror::Start(const Config& cfg, HMODULE self, const char* sourceName, bool routeToApp) {
    if (thread_) return true;
    cfg_ = cfg;
    source_ = sourceName ? sourceName : "LumaBridge";
    routeToApp_ = routeToApp;
    {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        correction_ = cfg.auraCorrection;
        disabledDevices_ = cfg.auraDisabledDevices;
        ++settingsVersion_;
    }
    stop_ = false;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) return false;

    // The worker holds its own reference on this module and exits through
    // FreeLibraryAndExitThread, so a FreeLibrary by the game without an SDK shutdown call
    // can't unmap code the worker is still running.
    HMODULE ref = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&AuraMirror::ThreadMain), &ref);
    if (ref && ref == GetModuleHandleW(nullptr)) {
        FreeLibrary(ref);  // running inside an exe (the app): nothing can unload it
        ref = nullptr;
    }
    module_ = ref;
    (void)self;

    thread_ = CreateThread(nullptr, 0, &AuraMirror::ThreadMain, this, 0, nullptr);
    if (!thread_) {
        LUMA_ERROR("Aura mirror: CreateThread failed, error %lu", GetLastError());
        if (ref) FreeLibrary(ref);
        CloseHandle(wake_);
        wake_ = nullptr;
        return false;
    }
    LUMA_INFO("Aura mirror: worker started for %s (max %d Hz)", source_.c_str(), cfg_.maxUpdateHz);
    return true;
}

void AuraMirror::Stop() {
    if (!thread_) return;
    stop_ = true;
    Wake();
    if (WaitForSingleObject(thread_, 5000) != WAIT_OBJECT_0)
        LUMA_WARN("Aura mirror: worker did not stop within 5s");
    CloseHandle(thread_);
    thread_ = nullptr;
    CloseHandle(wake_);
    wake_ = nullptr;
    std::lock_guard<std::mutex> lock(settingsMutex_);
    status_ = Status{};
    LUMA_INFO("Aura mirror: worker stopped");
}

void AuraMirror::SetStatic(Rgb c) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.SetStatic(c);
    }
    Wake();
}

void AuraMirror::Flash(Rgb c, int durationMs, int intervalMs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.StartFlash(c, durationMs, intervalMs, GetTickCount64());
    }
    Wake();
}

void AuraMirror::Pulse(Rgb c, int durationMs, int intervalMs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.StartPulse(c, durationMs, intervalMs, GetTickCount64());
    }
    Wake();
}

void AuraMirror::StopEffects() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.StopEffects();
    }
    Wake();
}

void AuraMirror::Save() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.Save();
}

void AuraMirror::Restore() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.Restore();
    }
    Wake();
}

void AuraMirror::SetCorrection(const ColorCorrection& cc) {
    {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        correction_ = cc;
        ++settingsVersion_;
    }
    Wake();
}

void AuraMirror::SetDisabledDevices(const std::vector<std::wstring>& names) {
    {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        disabledDevices_ = names;
        ++settingsVersion_;
    }
    Wake();
}

void AuraMirror::Rescan() {
    {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        rescan_ = true;
    }
    Wake();
}

AuraMirror::Status AuraMirror::GetStatus() const {
    std::lock_guard<std::mutex> lock(settingsMutex_);
    Status s = status_;
    s.running = thread_ != nullptr;
    return s;
}

bool AuraMirror::DeviceAllowed(const AuraDeviceInfo& d) const {
    if (!cfg_.auraDeviceTypes.empty() &&
        std::find(cfg_.auraDeviceTypes.begin(), cfg_.auraDeviceTypes.end(), d.type) ==
            cfg_.auraDeviceTypes.end())
        return false;
    for (const auto& ex : cfg_.auraExcludeNames)
        if (ContainsNoCase(d.name, ex)) return false;
    for (const auto& name : disabledDevices_)
        if (_wcsicmp(name.c_str(), d.name.c_str()) == 0) return false;
    return true;
}

DWORD WINAPI AuraMirror::ThreadMain(LPVOID param) {
    auto* self = static_cast<AuraMirror*>(param);
    HMODULE module = self->module_;
    self->Run();
    if (module) FreeLibraryAndExitThread(module, 0);
    return 0;
}

void AuraMirror::Run() {
    // A single-threaded apartment of our own (never the game's thread). The Aura SDK objects
    // are apartment-threaded; calling them from an MTA goes through cross-apartment
    // marshaling, the prime suspect for a crash seen on real hardware during Connect().
    // ASUS's samples all use an STA.
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrInit)) {
        LUMA_ERROR("Aura mirror: CoInitializeEx failed (hr=0x%08lX)",
                   static_cast<unsigned long>(hrInit));
        return;
    }

    const uint64_t framePeriodMs = std::max<uint64_t>(1, 1000 / cfg_.maxUpdateHz);
    const DWORD pid = GetCurrentProcessId();
    AuraBridge aura;
    uint64_t nextConnectAt = 0;
    uint64_t lastPushAt = 0;
    bool havePushed = false;
    uint32_t lastPushed = 0;
    uint64_t appliedSettings = ~0ull;
    bool wasRouted = false;
    bool toldNoApp = false;
    HWND app = nullptr;

    auto publish = [&] {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        status_.connected = aura.IsConnected();
        status_.routedToApp = app != nullptr;
        status_.devices = aura.Devices();
    };

    while (!stop_) {
        const uint64_t now = GetTickCount64();

        Rgb color;
        bool animating;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            color = state_.Evaluate(now);
            animating = state_.IsAnimating(now);
        }

        app = routeToApp_ ? FindApp() : nullptr;
        if (app) {
            // ---- Routed: the app owns Aura. Hand it back if we had it. ----
            if (!wasRouted) {
                LUMA_INFO("Aura mirror: LumaBridge app is running, routing %s frames to it",
                          source_.c_str());
                if (aura.IsConnected()) aura.Disconnect(true);
                havePushed = false;
                wasRouted = true;
                publish();
            }
            const uint32_t raw = ToAuraColor(color);  // the app applies calibration
            if (!havePushed || raw != lastPushed || now - lastPushAt >= kAppKeepAliveMs) {
                if (havePushed && raw != lastPushed && now - lastPushAt < framePeriodMs) {
                    Sleep(static_cast<DWORD>(framePeriodMs - (now - lastPushAt)));
                    continue;
                }
                SendToApp(app, ipc::MakeFrame(ipc::FrameKind::Color, pid, color.r, color.g, color.b,
                                              source_.c_str()));
                havePushed = true;
                lastPushed = raw;
                lastPushAt = GetTickCount64();
            }
            PumpingWait(wake_, animating ? static_cast<DWORD>(framePeriodMs) : kIdleWaitMs);
            continue;
        }
        if (routeToApp_ && !cfg_.auraDirectFromGames) {
            // Game DLL without the app: stay away from Aura (see Config::auraDirectFromGames).
            if (wasRouted || !toldNoApp) {
                LUMA_INFO("Aura mirror: LumaBridge app not running - start it to light Aura "
                          "(or set [Aura] DirectFromGames=1)");
                toldNoApp = true;
                wasRouted = false;
            }
            PumpingWait(wake_, kIdleWaitMs);
            continue;
        }
        if (wasRouted) {
            LUMA_INFO("Aura mirror: app gone, driving Aura directly");
            wasRouted = false;
            havePushed = false;
            nextConnectAt = 0;
        }

        // ---- Direct: talk to Aura ourselves. ----
        bool rescan;
        uint64_t settingsVersion;
        ColorCorrection cc;
        {
            std::lock_guard<std::mutex> lock(settingsMutex_);
            rescan = rescan_;
            rescan_ = false;
            settingsVersion = settingsVersion_;
            cc = correction_;
        }
        if (rescan && aura.IsConnected()) {
            aura.Disconnect(false);
            nextConnectAt = 0;
        }

        if (!aura.IsConnected() && now >= nextConnectAt) {
            if (aura.Connect()) {
                havePushed = false;
                appliedSettings = ~0ull;
            } else {
                nextConnectAt = now + kReconnectDelayMs;
            }
            publish();
        }

        if (aura.IsConnected() && settingsVersion != appliedSettings) {
            std::lock_guard<std::mutex> lock(settingsMutex_);
            const auto& devs = aura.Devices();
            for (size_t i = 0; i < devs.size(); ++i) aura.SetSelected(i, DeviceAllowed(devs[i]));
            appliedSettings = settingsVersion;
            havePushed = false;  // calibration or device set changed: repaint
        }

        const uint32_t out = ToAuraColor(ApplyCorrection(cc, color));
        if (aura.IsConnected() && (!havePushed || out != lastPushed)) {
            // Rate limit: coalesce bursts of game calls into one frame per period.
            if (havePushed && now - lastPushAt < framePeriodMs) {
                Sleep(static_cast<DWORD>(framePeriodMs - (now - lastPushAt)));
                continue;  // re-evaluate with the freshest state
            }
            if (aura.SetAll(out)) {
                havePushed = true;
                lastPushed = out;
                lastPushAt = GetTickCount64();
            } else {
                LUMA_WARN("Aura mirror: push failed, reconnecting in %llu ms",
                          static_cast<unsigned long long>(kReconnectDelayMs));
                aura.Disconnect(false);
                nextConnectAt = GetTickCount64() + kReconnectDelayMs;
                publish();
            }
        }

        // Animations need a steady tick; otherwise sleep until something changes
        // (or periodically, to retry a failed Aura connection / notice the app).
        PumpingWait(wake_, animating ? static_cast<DWORD>(framePeriodMs) : kIdleWaitMs);
    }

    if (wasRouted && app)
        SendToApp(app, ipc::MakeFrame(ipc::FrameKind::Release, pid, 0, 0, 0, source_.c_str()));
    aura.Disconnect(cfg_.releaseControlOnShutdown);
    publish();
    CoUninitialize();
}

}  // namespace luma
