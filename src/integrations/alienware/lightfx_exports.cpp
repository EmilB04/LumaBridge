// Alienware LightFX emulator: a stand-in LightFX.dll (normally installed into System32 by
// Alienware Command Center) reporting one virtual light that is mirrored onto Aura.
#include <windows.h>

#include <cstring>
#include <mutex>

#include "aura_mirror.h"
#include "config.h"
#include "lightfx_state.h"
#include "log.h"
#include "module_bootstrap.h"

using namespace luma;
using namespace luma::lightfx;

#define LFX_CALL __cdecl

namespace luma {
HMODULE g_selfModule = nullptr;  // set in DllMain
}

namespace {

constexpr char kDeviceDescription[] = "LumaBridge Aura";
constexpr char kLightDescription[] = "Ambient";
constexpr char kVersion[] = "2.0.0.0 (LumaBridge)";

std::once_flag g_bootstrapOnce;
Config g_cfg;

std::mutex g_mutex;  // guards everything below
bool g_initialized = false;
AuraMirror g_mirror;
State g_state;

void Bootstrap() {
    std::call_once(g_bootstrapOnce,
                   [] { g_cfg = BootstrapModule(g_selfModule, L"lightfx", "LightFX emulator"); });
}

bool ValidLight(unsigned int dev, unsigned int light) { return dev == 0 && light == 0; }

LFX_RESULT CopyString(const char* src, char* dst, unsigned int size) {
    if (!dst) return LFX_FAILURE;
    const size_t n = std::strlen(src);
    if (size <= n) return LFX_ERROR_BUFFSIZE;
    std::memcpy(dst, src, n + 1);
    return LFX_SUCCESS;
}

}  // namespace

#define LFX_REQUIRE_INIT()                               \
    Bootstrap();                                         \
    std::lock_guard<std::mutex> lock(g_mutex);           \
    if (!g_initialized) return LFX_ERROR_NOINIT

extern "C" LFX_RESULT LFX_CALL LFX_Initialize() {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        g_initialized = true;
        if (g_cfg.auraEnabled) g_mirror.Start(g_cfg, g_selfModule, "Alienware LightFX");
        LUMA_INFO("LightFX: Initialize (aura %s)", g_mirror.IsRunning() ? "running" : "off");
    }
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_Release() {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) {
        LUMA_INFO("LightFX: Release");
        g_mirror.Stop();
        g_initialized = false;
    }
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_Reset() {
    LFX_REQUIRE_INIT();
    g_state.Reset();
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_Update() {
    LFX_REQUIRE_INIT();
    if (auto cmd = g_state.Update()) {
        if (cmd->kind == Command::Kind::Pulse)
            g_mirror.Pulse(cmd->color, 0 /* until next update */, cmd->periodMs);
        else
            g_mirror.SetStatic(cmd->color);
    }
    return LFX_SUCCESS;
}

// "Make the current state the power-on default" -- nothing persistent to write.
extern "C" LFX_RESULT LFX_CALL LFX_UpdateDefault() { return LFX_Update(); }

extern "C" LFX_RESULT LFX_CALL LFX_GetNumDevices(unsigned int* numDevices) {
    LFX_REQUIRE_INIT();
    if (!numDevices) return LFX_FAILURE;
    *numDevices = 1;
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_GetDeviceDescription(unsigned int devIndex, char* devDesc,
                                                        unsigned int devDescSize,
                                                        unsigned char* devType) {
    LFX_REQUIRE_INIT();
    if (devIndex != 0) return LFX_ERROR_NODEVS;
    if (devType) *devType = LFX_DEVTYPE_DESKTOP;
    return CopyString(kDeviceDescription, devDesc, devDescSize);
}

extern "C" LFX_RESULT LFX_CALL LFX_GetNumLights(unsigned int devIndex, unsigned int* numLights) {
    LFX_REQUIRE_INIT();
    if (devIndex != 0) return LFX_ERROR_NODEVS;
    if (!numLights) return LFX_FAILURE;
    *numLights = 1;
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_GetLightDescription(unsigned int devIndex, unsigned int lightIndex,
                                                       char* lightDesc, unsigned int lightDescSize) {
    LFX_REQUIRE_INIT();
    if (!ValidLight(devIndex, lightIndex)) return LFX_ERROR_NOLIGHTS;
    return CopyString(kLightDescription, lightDesc, lightDescSize);
}

// Center of the 3x3x3 location grid: the light "is everywhere".
extern "C" LFX_RESULT LFX_CALL LFX_GetLightLocation(unsigned int devIndex, unsigned int lightIndex,
                                                    LFX_POSITION* lightLoc) {
    LFX_REQUIRE_INIT();
    if (!ValidLight(devIndex, lightIndex)) return LFX_ERROR_NOLIGHTS;
    if (!lightLoc) return LFX_FAILURE;
    *lightLoc = LFX_POSITION{1, 1, 1};
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_GetLightColor(unsigned int devIndex, unsigned int lightIndex,
                                                 LFX_COLOR* lightCol) {
    LFX_REQUIRE_INIT();
    if (!ValidLight(devIndex, lightIndex)) return LFX_ERROR_NOLIGHTS;
    if (!lightCol) return LFX_FAILURE;
    Rgb c = g_state.CurrentColor();
    *lightCol = LFX_COLOR{c.r, c.g, c.b, 0xFF};
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_SetLightColor(unsigned int devIndex, unsigned int lightIndex,
                                                 const LFX_COLOR* lightCol) {
    LFX_REQUIRE_INIT();
    if (!ValidLight(devIndex, lightIndex)) return LFX_ERROR_NOLIGHTS;
    if (!lightCol) return LFX_FAILURE;
    g_state.SetColor(FromStruct(*lightCol));
    return LFX_SUCCESS;
}

// Every location mask includes our single all-covering light.
extern "C" LFX_RESULT LFX_CALL LFX_Light(unsigned int /*locationMask*/, unsigned int colorVal) {
    LFX_REQUIRE_INIT();
    g_state.SetColor(FromPacked(colorVal));
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_SetLightActionColor(unsigned int devIndex, unsigned int lightIndex,
                                                       unsigned int actionType,
                                                       const LFX_COLOR* primaryCol) {
    LFX_REQUIRE_INIT();
    if (!ValidLight(devIndex, lightIndex)) return LFX_ERROR_NOLIGHTS;
    if (!primaryCol) return LFX_FAILURE;
    g_state.Action(actionType, FromStruct(*primaryCol), std::nullopt);
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_SetLightActionColorEx(unsigned int devIndex,
                                                         unsigned int lightIndex,
                                                         unsigned int actionType,
                                                         const LFX_COLOR* primaryCol,
                                                         const LFX_COLOR* secondaryCol) {
    LFX_REQUIRE_INIT();
    if (!ValidLight(devIndex, lightIndex)) return LFX_ERROR_NOLIGHTS;
    if (!primaryCol || !secondaryCol) return LFX_FAILURE;
    g_state.Action(actionType, FromStruct(*primaryCol), FromStruct(*secondaryCol));
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_ActionColor(unsigned int /*locationMask*/, unsigned int actionType,
                                               unsigned int primaryCol) {
    LFX_REQUIRE_INIT();
    g_state.Action(actionType, FromPacked(primaryCol), std::nullopt);
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_ActionColorEx(unsigned int /*locationMask*/, unsigned int actionType,
                                                 unsigned int primaryCol, unsigned int secondaryCol) {
    LFX_REQUIRE_INIT();
    g_state.Action(actionType, FromPacked(primaryCol), FromPacked(secondaryCol));
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_SetTiming(int newTiming) {
    LFX_REQUIRE_INIT();
    g_state.SetTiming(newTiming);
    return LFX_SUCCESS;
}

extern "C" LFX_RESULT LFX_CALL LFX_GetVersion(char* version, unsigned int versionSize) {
    LFX_REQUIRE_INIT();
    return CopyString(kVersion, version, versionSize);
}
