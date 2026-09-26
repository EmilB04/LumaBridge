#include "aura_mirror.h"

#include <objbase.h>

#include <algorithm>
#include <cwctype>

#include "aura_bridge.h"
#include "log.h"

namespace luma {
namespace {

constexpr uint64_t kReconnectDelayMs = 5000;
constexpr DWORD kIdleWaitMs = 1000;

bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
    return it != hay.end();
}

}  // namespace

bool AuraMirror::Start(const Config& cfg, HMODULE self) {
    if (thread_) return true;
    cfg_ = cfg;
    stop_ = false;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) return false;

    // The worker holds its own reference on this DLL and exits through
    // FreeLibraryAndExitThread, so a FreeLibrary by the game without LogiLedShutdown can't
    // unmap code the worker is still running.
    HMODULE ref = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&AuraMirror::ThreadMain), &ref);
    module_ = ref ? ref : self;

    thread_ = CreateThread(nullptr, 0, &AuraMirror::ThreadMain, this, 0, nullptr);
    if (!thread_) {
        LUMA_ERROR("Aura mirror: CreateThread failed, error %lu", GetLastError());
        if (ref) FreeLibrary(ref);
        CloseHandle(wake_);
        wake_ = nullptr;
        return false;
    }
    LUMA_INFO("Aura mirror: worker started (max %d Hz)", cfg_.maxUpdateHz);
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

DWORD WINAPI AuraMirror::ThreadMain(LPVOID param) {
    auto* self = static_cast<AuraMirror*>(param);
    HMODULE module = self->module_;
    self->Run();
    FreeLibraryAndExitThread(module, 0);
    return 0;  // unreachable
}

void AuraMirror::Run() {
    // Our own MTA: never touches the game's COM apartment choices.
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrInit)) {
        LUMA_ERROR("Aura mirror: CoInitializeEx failed (hr=0x%08lX)",
                   static_cast<unsigned long>(hrInit));
        return;
    }

    const Config& cfg = cfg_;
    auto filter = [&cfg](const AuraDeviceInfo& d) {
        if (!cfg.auraDeviceTypes.empty() &&
            std::find(cfg.auraDeviceTypes.begin(), cfg.auraDeviceTypes.end(), d.type) ==
                cfg.auraDeviceTypes.end())
            return false;
        for (const auto& ex : cfg.auraExcludeNames)
            if (ContainsNoCase(d.name, ex)) return false;
        return true;
    };

    const uint64_t framePeriodMs = std::max<uint64_t>(1, 1000 / cfg.maxUpdateHz);
    AuraBridge aura;
    uint64_t nextConnectAt = 0;
    uint64_t lastPushAt = 0;
    bool havePushed = false;
    uint32_t lastPushed = 0;

    while (!stop_) {
        const uint64_t now = GetTickCount64();

        if (!aura.IsConnected() && now >= nextConnectAt) {
            if (aura.Connect(filter)) {
                havePushed = false;  // force a push of the current color
            } else {
                nextConnectAt = now + kReconnectDelayMs;
            }
        }

        Rgb color;
        bool animating;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            color = state_.Evaluate(now);
            animating = state_.IsAnimating(now);
        }
        const uint32_t out = ToAuraColor(ApplyCorrection(cfg.auraCorrection, color));

        if (aura.IsConnected() && (!havePushed || out != lastPushed)) {
            // Rate limit: coalesce bursts of game calls into one frame per period.
            if (havePushed && now - lastPushAt < framePeriodMs) {
                Sleep(static_cast<DWORD>(framePeriodMs - (now - lastPushAt)));
                continue;  // re-evaluate with the freshest state
            }
            if (aura.SetAll(out)) {
                LUMA_DEBUG("Aura mirror: pushed 0x%06lX", static_cast<unsigned long>(out));
                havePushed = true;
                lastPushed = out;
                lastPushAt = GetTickCount64();
            } else {
                LUMA_WARN("Aura mirror: push failed, reconnecting in %llu ms",
                          static_cast<unsigned long long>(kReconnectDelayMs));
                aura.Disconnect(false);
                nextConnectAt = GetTickCount64() + kReconnectDelayMs;
            }
        }

        // Animations need a steady tick; otherwise sleep until the game changes something
        // (or periodically, to retry a failed Aura connection).
        WaitForSingleObject(wake_, animating ? static_cast<DWORD>(framePeriodMs) : kIdleWaitMs);
    }

    aura.Disconnect(cfg.releaseControlOnShutdown);
    CoUninitialize();
}

}  // namespace luma
