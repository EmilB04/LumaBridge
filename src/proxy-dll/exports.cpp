// The exported LogiLed* functions. Every call is forwarded to the real Logitech DLL first
// (so Logitech devices keep working), then fanned out to the Aura mirror.
//
// Return values: a call reports success if either the real SDK or the Aura mirror accepted
// it, so a game keeps sending lighting even when G HUB isn't running.
#include <windows.h>

#include <iterator>

#include <mutex>
#include <string>
#include <type_traits>

#include "aura_mirror.h"
#include "color.h"
#include "config.h"
#include "log.h"
#include "logiled_api.h"
#include "real_logiled.h"

namespace luma {

HMODULE g_selfModule = nullptr;  // set in DllMain

namespace {

std::once_flag g_bootstrapOnce;
Config g_cfg;
std::wstring g_selfPath;

std::mutex g_lifecycleMutex;  // serialises Init / Shutdown
AuraMirror g_mirror;

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? static_cast<char>(c) : '?';
    return s;
}

// Loads config, opens the log and loads the real DLL. Runs on the first LogiLed* call,
// never from DllMain (LoadLibrary under the loader lock is asking for deadlocks).
void Bootstrap() {
    std::call_once(g_bootstrapOnce, [] {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(g_selfModule, buf, static_cast<DWORD>(std::size(buf)));
        if (n > 0 && n < std::size(buf)) g_selfPath.assign(buf, n);
        std::wstring dir = g_selfPath.substr(0, g_selfPath.find_last_of(L"\\/"));

        g_cfg = LoadConfig(dir);
        log::Init(g_cfg.logFile, g_cfg.logLevel);

        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        LUMA_INFO("==== LumaBridge proxy loaded into %s (pid %lu) ====", Narrow(exe).c_str(),
                  GetCurrentProcessId());
        LUMA_INFO("proxy: %s", Narrow(g_selfPath).c_str());
        LUMA_INFO("config: %s", g_cfg.sourcePath.empty() ? "(none found, using defaults)"
                                                         : Narrow(g_cfg.sourcePath).c_str());

        if (g_cfg.passthrough)
            real::Load(g_cfg.realDllPath, g_selfPath);
        else
            LUMA_INFO("Logitech: pass-through disabled in config");
    });
}

bool MirrorActive() { return g_mirror.IsRunning(); }

// Value a pass-through export returns when the real DLL (or that export) is missing.
template <typename T>
T Fallback() {
    if constexpr (std::is_void_v<T>)
        return;
    else
        return T{};
}

bool StartSession(bool realOk) {
    if (g_cfg.auraEnabled) g_mirror.Start(g_cfg, g_selfModule);
    LUMA_INFO("LogiLedInit: real=%s aura=%s", realOk ? "ok" : "unavailable",
              MirrorActive() ? "running" : "off");
    return realOk || MirrorActive();
}

}  // namespace
}  // namespace luma

using namespace luma;

// ---- Pure pass-through exports -------------------------------------------------------

#define LUMA_FORWARD(ret, name, params, args)      \
    extern "C" ret LOGILED_CALL name params {      \
        Bootstrap();                               \
        if (real::name) return real::name args;    \
        return Fallback<ret>();                    \
    }
LOGILED_PASSTHROUGH(LUMA_FORWARD)
#undef LUMA_FORWARD

// ---- Intercepted exports -------------------------------------------------------------

extern "C" bool LOGILED_CALL LogiLedInit() {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    bool realOk = real::LogiLedInit && real::LogiLedInit();
    return StartSession(realOk);
}

extern "C" bool LOGILED_CALL LogiLedInitWithName(const char name[]) {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    LUMA_INFO("LogiLedInitWithName(\"%s\")", name ? name : "(null)");
    bool realOk = false;
    if (real::LogiLedInitWithName)
        realOk = real::LogiLedInitWithName(name);
    else if (real::LogiLedInit)  // SDKs before 8.87 only have LogiLedInit
        realOk = real::LogiLedInit();
    return StartSession(realOk);
}

extern "C" void LOGILED_CALL LogiLedShutdown() {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    LUMA_INFO("LogiLedShutdown");
    g_mirror.Stop();
    if (real::LogiLedShutdown) real::LogiLedShutdown();
    // The real DLL stays loaded: some games call LogiLedInit again later.
}

extern "C" bool LOGILED_CALL LogiLedSetLighting(int redPercentage, int greenPercentage,
                                                int bluePercentage) {
    Bootstrap();
    bool r = real::LogiLedSetLighting &&
             real::LogiLedSetLighting(redPercentage, greenPercentage, bluePercentage);
    if (MirrorActive()) g_mirror.SetStatic(FromPercent(redPercentage, greenPercentage, bluePercentage));
    return r || MirrorActive();
}

extern "C" bool LOGILED_CALL LogiLedSetLightingFromBitmap(unsigned char bitmap[]) {
    Bootstrap();
    bool r = real::LogiLedSetLightingFromBitmap && real::LogiLedSetLightingFromBitmap(bitmap);
    if (MirrorActive() && bitmap)
        g_mirror.SetStatic(ReduceBitmap(bitmap, LOGI_LED_BITMAP_SIZE, g_cfg.bitmapReduce));
    return r || MirrorActive();
}

extern "C" bool LOGILED_CALL LogiLedSetLightingForTargetZone(int deviceType, int zone,
                                                             int redPercentage,
                                                             int greenPercentage,
                                                             int bluePercentage) {
    Bootstrap();
    bool r = real::LogiLedSetLightingForTargetZone &&
             real::LogiLedSetLightingForTargetZone(deviceType, zone, redPercentage,
                                                   greenPercentage, bluePercentage);
    if (MirrorActive())
        g_mirror.SetStatic(FromPercent(redPercentage, greenPercentage, bluePercentage));
    return r || MirrorActive();
}

extern "C" bool LOGILED_CALL LogiLedFlashLighting(int redPercentage, int greenPercentage,
                                                  int bluePercentage, int milliSecondsDuration,
                                                  int milliSecondsInterval) {
    Bootstrap();
    bool r = real::LogiLedFlashLighting &&
             real::LogiLedFlashLighting(redPercentage, greenPercentage, bluePercentage,
                                        milliSecondsDuration, milliSecondsInterval);
    if (MirrorActive())
        g_mirror.Flash(FromPercent(redPercentage, greenPercentage, bluePercentage),
                       milliSecondsDuration, milliSecondsInterval);
    return r || MirrorActive();
}

extern "C" bool LOGILED_CALL LogiLedPulseLighting(int redPercentage, int greenPercentage,
                                                  int bluePercentage, int milliSecondsDuration,
                                                  int milliSecondsInterval) {
    Bootstrap();
    bool r = real::LogiLedPulseLighting &&
             real::LogiLedPulseLighting(redPercentage, greenPercentage, bluePercentage,
                                        milliSecondsDuration, milliSecondsInterval);
    if (MirrorActive())
        g_mirror.Pulse(FromPercent(redPercentage, greenPercentage, bluePercentage),
                       milliSecondsDuration, milliSecondsInterval);
    return r || MirrorActive();
}

extern "C" bool LOGILED_CALL LogiLedStopEffects() {
    Bootstrap();
    bool r = real::LogiLedStopEffects && real::LogiLedStopEffects();
    if (MirrorActive()) g_mirror.StopEffects();
    return r || MirrorActive();
}

extern "C" bool LOGILED_CALL LogiLedSaveCurrentLighting() {
    Bootstrap();
    bool r = real::LogiLedSaveCurrentLighting && real::LogiLedSaveCurrentLighting();
    if (MirrorActive()) g_mirror.Save();
    return r || MirrorActive();
}

extern "C" bool LOGILED_CALL LogiLedRestoreLighting() {
    Bootstrap();
    bool r = real::LogiLedRestoreLighting && real::LogiLedRestoreLighting();
    if (MirrorActive()) g_mirror.Restore();
    return r || MirrorActive();
}
