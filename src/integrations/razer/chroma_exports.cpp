// Razer Chroma SDK emulator: a stand-in RzChromaSDK64.dll / RzChromaSDK.dll that accepts a
// game's Chroma effects and mirrors them onto Aura devices. No Razer hardware or Synapse
// needed. Games address Chroma effects to device *classes* (keyboard, mouse, Chroma Link...),
// not to a specific model, so no particular Razer product has to be impersonated.
//
// There is no pass-through: this DLL replaces the Razer one.
#include <windows.h>

#include <objbase.h>

#include <cstring>
#include <map>
#include <mutex>
#include <optional>

#include "aura_mirror.h"
#include "chroma_translate.h"
#include "chroma_types.h"
#include "config.h"
#include "log.h"
#include "module_bootstrap.h"

using namespace luma;
using namespace luma::chroma;

#define CHROMA_CALL __cdecl

namespace luma {
HMODULE g_selfModule = nullptr;  // set in DllMain
}

namespace {

struct GuidLess {
    bool operator()(const GUID& a, const GUID& b) const { return std::memcmp(&a, &b, sizeof a) < 0; }
};

struct StoredEffect {
    DeviceClass device;
    std::optional<Rgb> color;  // nullopt: accepted but not emulated (e.g. wave)
};

std::once_flag g_bootstrapOnce;
Config g_cfg;

std::mutex g_mutex;  // guards everything below
bool g_initialized = false;
AuraMirror g_mirror;
AmbientSelector g_selector;
std::map<GUID, StoredEffect, GuidLess> g_effects;

AmbientSource ParseAmbientSource(const std::wstring& s) {
    static const struct {
        const wchar_t* name;
        AmbientSource src;
    } kNames[] = {
        {L"keyboard", AmbientSource::Keyboard}, {L"mouse", AmbientSource::Mouse},
        {L"headset", AmbientSource::Headset},   {L"mousepad", AmbientSource::Mousepad},
        {L"keypad", AmbientSource::Keypad},     {L"chromalink", AmbientSource::ChromaLink},
    };
    for (const auto& n : kNames)
        if (_wcsicmp(s.c_str(), n.name) == 0) return n.src;
    return AmbientSource::Auto;
}

void Bootstrap() {
    std::call_once(g_bootstrapOnce, [] {
        g_cfg = BootstrapModule(g_selfModule, L"chroma", "Chroma emulator");
        g_selector = AmbientSelector(ParseAmbientSource(g_cfg.chromaAmbientSource));
    });
}

// Must hold g_mutex.
void ApplyLocked(DeviceClass dc, const std::optional<Rgb>& color) {
    if (!color) return;
    if (auto ambient = g_selector.Update(dc, *color)) {
        LUMA_DEBUG("Chroma: %s -> #%02X%02X%02X", DeviceClassName(dc), ambient->r, ambient->g,
                   ambient->b);
        g_mirror.SetStatic(*ambient);
    }
}

// Common path for every Create*Effect: apply now (effectId == null) or store the effect
// for a later SetEffect.
RZRESULT CreateCommon(DeviceClass dc, int type, const std::optional<Rgb>& color, GUID* effectId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return RZRESULT_SERVICE_NOT_ACTIVE;
    if (!color) LUMA_DEBUG("Chroma: %s effect type %d not emulated", DeviceClassName(dc), type);

    if (!effectId) {
        ApplyLocked(dc, color);
        return RZRESULT_SUCCESS;
    }
    if (FAILED(CoCreateGuid(effectId))) return RZRESULT_FAILED;
    g_effects[*effectId] = StoredEffect{dc, color};
    return RZRESULT_SUCCESS;
}

RZRESULT CreateDeviceEffect(DeviceClass dc, int type, void* param, GUID* effectId) {
    Bootstrap();
    return CreateCommon(dc, type, TranslateEffect(dc, type, param, g_cfg.bitmapReduce), effectId);
}

}  // namespace

// ---- Exports -------------------------------------------------------------------------

extern "C" RZRESULT CHROMA_CALL Init() {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return RZRESULT_SUCCESS;
    g_initialized = true;
    if (g_cfg.auraEnabled) g_mirror.Start(g_cfg, g_selfModule, "Razer Chroma");
    LUMA_INFO("Chroma: Init (aura %s)", g_mirror.IsRunning() ? "running" : "off");
    return RZRESULT_SUCCESS;
}

// SDK 3.x: InitSDK(ChromaSDK::APPINFOTYPE*) registers the app's name/author with Synapse.
// Nothing to register here.
extern "C" RZRESULT CHROMA_CALL InitSDK(void* /*appInfo*/) { return Init(); }

extern "C" RZRESULT CHROMA_CALL UnInit() {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return RZRESULT_SUCCESS;
    LUMA_INFO("Chroma: UnInit");
    g_mirror.Stop();
    g_effects.clear();
    g_initialized = false;
    return RZRESULT_SUCCESS;
}

extern "C" RZRESULT CHROMA_CALL CreateEffect(GUID /*deviceId*/, int effect, void* param,
                                             GUID* effectId) {
    Bootstrap();
    // Generic effects are addressed to a specific device GUID; only the device-independent
    // types are emulated and they count as keyboard-class for ambient selection.
    return CreateCommon(DeviceClass::Keyboard, effect, TranslateGenericEffect(effect, param),
                        effectId);
}

extern "C" RZRESULT CHROMA_CALL CreateKeyboardEffect(int effect, void* param, GUID* effectId) {
    return CreateDeviceEffect(DeviceClass::Keyboard, effect, param, effectId);
}

extern "C" RZRESULT CHROMA_CALL CreateMouseEffect(int effect, void* param, GUID* effectId) {
    return CreateDeviceEffect(DeviceClass::Mouse, effect, param, effectId);
}

extern "C" RZRESULT CHROMA_CALL CreateHeadsetEffect(int effect, void* param, GUID* effectId) {
    return CreateDeviceEffect(DeviceClass::Headset, effect, param, effectId);
}

extern "C" RZRESULT CHROMA_CALL CreateMousepadEffect(int effect, void* param, GUID* effectId) {
    return CreateDeviceEffect(DeviceClass::Mousepad, effect, param, effectId);
}

extern "C" RZRESULT CHROMA_CALL CreateKeypadEffect(int effect, void* param, GUID* effectId) {
    return CreateDeviceEffect(DeviceClass::Keypad, effect, param, effectId);
}

extern "C" RZRESULT CHROMA_CALL CreateChromaLinkEffect(int effect, void* param, GUID* effectId) {
    return CreateDeviceEffect(DeviceClass::ChromaLink, effect, param, effectId);
}

extern "C" RZRESULT CHROMA_CALL SetEffect(GUID effectId) {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return RZRESULT_SERVICE_NOT_ACTIVE;
    auto it = g_effects.find(effectId);
    if (it == g_effects.end()) return RZRESULT_NOT_FOUND;
    ApplyLocked(it->second.device, it->second.color);
    return RZRESULT_SUCCESS;
}

extern "C" RZRESULT CHROMA_CALL DeleteEffect(GUID effectId) {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_effects.erase(effectId) ? RZRESULT_SUCCESS : RZRESULT_NOT_FOUND;
}

// Synapse posts WM_CHROMA_EVENT for "Chroma app switched" etc. We never send any.
extern "C" RZRESULT CHROMA_CALL RegisterEventNotification(HWND /*hWnd*/) {
    return RZRESULT_SUCCESS;
}

extern "C" RZRESULT CHROMA_CALL UnregisterEventNotification() { return RZRESULT_SUCCESS; }

// Games ask about specific Razer model GUIDs. With ReportDevicesConnected every model is
// "connected", which is what makes a game light up a keyboard you don't have.
extern "C" RZRESULT CHROMA_CALL QueryDevice(GUID /*deviceId*/, DeviceInfo& info) {
    Bootstrap();
    info.deviceType = 1;  // DEVICE_KEYBOARD; a model GUID -> type table is not needed so far
    info.connected = g_cfg.chromaReportDevicesConnected ? 1 : 0;
    return RZRESULT_SUCCESS;
}
