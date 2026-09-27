// Corsair iCUE SDK (CUE SDK 2.x / 3.x) emulator: a stand-in CUESDK.x64_2019.dll (and the
// other CUESDK*.dll names) that reports a virtual Corsair keyboard + mouse to the game and
// mirrors whatever the game paints onto Aura. No Corsair hardware or iCUE needed.
//
// Games ship the CUESDK DLL in their own folder, so installation is a per-game file
// replacement (scripts/Install-SdkEmulators.ps1 -Corsair -GameDir ...).
#include <windows.h>

#include <mutex>
#include <vector>

#include "aura_mirror.h"
#include "config.h"
#include "corsair_types.h"
#include "fake_devices.h"
#include "log.h"
#include "module_bootstrap.h"

using namespace luma;
using namespace luma::corsair;

#define CORSAIR_CALL __cdecl

namespace luma {
HMODULE g_selfModule = nullptr;  // set in DllMain
}

namespace {

using AsyncCallback = void(CORSAIR_CALL*)(void* context, bool result, CorsairError error);

std::once_flag g_bootstrapOnce;
Config g_cfg;

// Built once in Bootstrap and never modified afterwards: the pointers handed to the game
// (CorsairGetDeviceInfo / CorsairGetLedPositions) stay valid for the life of the process.
std::vector<FakeDevice> g_devices;
std::vector<CorsairDeviceInfo> g_infos;
std::vector<std::vector<CorsairLedPosition>> g_positions;
std::vector<CorsairLedPositions> g_positionSets;
std::vector<std::string> g_deviceIds;

std::mutex g_mutex;  // guards everything below
bool g_handshakeDone = false;
AuraMirror g_mirror;
LedFrame g_frame;
std::vector<CorsairLedColor> g_buffer;  // CorsairSetLedsColorsBufferByDeviceIndex
Rgb g_lastAmbient{};
bool g_haveAmbient = false;

thread_local CorsairError t_lastError = CE_Success;

void Bootstrap() {
    std::call_once(g_bootstrapOnce, [] {
        g_cfg = BootstrapModule(g_selfModule, L"corsair", "Corsair iCUE emulator");
        g_devices = BuildFakeDevices();
        g_positions.resize(g_devices.size());
        for (size_t i = 0; i < g_devices.size(); ++i) {
            g_deviceIds.push_back("{lumabridge-corsair-" + std::to_string(i) + "}");
            for (const auto& l : g_devices[i].leds)
                g_positions[i].push_back(CorsairLedPosition{l.id, l.top, l.left, l.height, l.width});
        }
        for (size_t i = 0; i < g_devices.size(); ++i) {
            g_positionSets.push_back(CorsairLedPositions{static_cast<int>(g_positions[i].size()),
                                                         g_positions[i].data()});
            CorsairDeviceInfo info{};
            info.type = g_devices[i].type;
            info.model = g_devices[i].model;
            info.physicalLayout = g_devices[i].physicalLayout;
            info.logicalLayout = g_devices[i].type == CDT_Keyboard ? CLL_US_Int : CLL_Invalid;
            info.capsMask = CDC_Lighting;
            info.ledsCount = static_cast<int>(g_devices[i].leds.size());
            info.channels = CorsairChannelsInfo{0, nullptr};
            info.deviceId = g_deviceIds[i].c_str();
            g_infos.push_back(info);
            LUMA_INFO("Corsair: virtual device %u: %s, %d LEDs", static_cast<unsigned>(i), info.model,
                      info.ledsCount);
        }
    });
}

bool Fail(CorsairError e) {
    t_lastError = e;
    return false;
}

bool Ok() {
    t_lastError = CE_Success;
    return true;
}

bool ValidDevice(int index) { return index >= 0 && index < static_cast<int>(g_devices.size()); }

// Must hold g_mutex.
void PushAmbientLocked() {
    Rgb c = g_frame.Reduce(g_cfg.bitmapReduce);
    if (g_haveAmbient && c == g_lastAmbient) return;
    g_lastAmbient = c;
    g_haveAmbient = true;
    g_mirror.SetStatic(c);
}

// Must hold g_mutex.
bool ApplyLocked(int size, const CorsairLedColor* colors) {
    if (size < 0 || (size > 0 && !colors)) return Fail(CE_InvalidArguments);
    for (int i = 0; i < size; ++i) g_frame.Set(colors[i].ledId, colors[i].r, colors[i].g, colors[i].b);
    PushAmbientLocked();
    return Ok();
}

struct AsyncJob {
    AsyncCallback callback;
    void* context;
    bool result;
    CorsairError error;
};

void CALLBACK RunAsyncJob(PTP_CALLBACK_INSTANCE, void* p) {
    auto* j = static_cast<AsyncJob*>(p);
    j->callback(j->context, j->result, j->error);
    delete j;
}

// The real SDK reports completion from its own thread; do the same so a game that holds a
// lock around the call can't deadlock on a re-entrant callback.
void CompleteAsync(AsyncCallback cb, void* context, bool result) {
    if (!cb) return;
    auto* job = new AsyncJob{cb, context, result, t_lastError};
    if (!TrySubmitThreadpoolCallback(&RunAsyncJob, job, nullptr)) {
        delete job;
        cb(context, result, t_lastError);
    }
}

}  // namespace

// ---- Exports -------------------------------------------------------------------------

extern "C" CorsairProtocolDetails CORSAIR_CALL CorsairPerformProtocolHandshake() {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_handshakeDone) {
        g_handshakeDone = true;
        if (g_cfg.auraEnabled) g_mirror.Start(g_cfg, g_selfModule, "Corsair iCUE");
        LUMA_INFO("Corsair: handshake (aura %s)", g_mirror.IsRunning() ? "running" : "off");
    }
    t_lastError = CE_Success;
    // Same version on both sides and no breaking changes: the game proceeds.
    return CorsairProtocolDetails{"3.0.378", "3.0.378 (LumaBridge)", 16, 16, false};
}

extern "C" CorsairError CORSAIR_CALL CorsairGetLastError() { return t_lastError; }

extern "C" int CORSAIR_CALL CorsairGetDeviceCount() {
    Bootstrap();
    t_lastError = CE_Success;
    return static_cast<int>(g_devices.size());
}

extern "C" CorsairDeviceInfo* CORSAIR_CALL CorsairGetDeviceInfo(int deviceIndex) {
    Bootstrap();
    if (!ValidDevice(deviceIndex)) {
        Fail(CE_InvalidArguments);
        return nullptr;
    }
    Ok();
    return &g_infos[deviceIndex];
}

// SDK 2.x semantics: positions of the (first) keyboard.
extern "C" CorsairLedPositions* CORSAIR_CALL CorsairGetLedPositions() {
    Bootstrap();
    Ok();
    return &g_positionSets[0];
}

extern "C" CorsairLedPositions* CORSAIR_CALL CorsairGetLedPositionsByDeviceIndex(int deviceIndex) {
    Bootstrap();
    if (!ValidDevice(deviceIndex)) {
        Fail(CE_InvalidArguments);
        return nullptr;
    }
    Ok();
    return &g_positionSets[deviceIndex];
}

extern "C" int CORSAIR_CALL CorsairGetLedIdForKeyName(char keyName) {
    Bootstrap();
    int id = LedIdForKeyName(g_devices, keyName);
    if (!id) Fail(CE_InvalidArguments); else Ok();
    return id;
}

extern "C" bool CORSAIR_CALL CorsairSetLedsColors(int size, CorsairLedColor* ledsColors) {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_mutex);
    return ApplyLocked(size, ledsColors);
}

extern "C" bool CORSAIR_CALL CorsairSetLedsColorsAsync(int size, CorsairLedColor* ledsColors,
                                                       AsyncCallback callback, void* context) {
    Bootstrap();
    bool r;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        r = ApplyLocked(size, ledsColors);
    }
    CompleteAsync(callback, context, r);
    return r;
}

extern "C" bool CORSAIR_CALL CorsairSetLedsColorsBufferByDeviceIndex(int deviceIndex, int size,
                                                                     CorsairLedColor* ledsColors) {
    Bootstrap();
    if (!ValidDevice(deviceIndex) || size < 0 || (size > 0 && !ledsColors))
        return Fail(CE_InvalidArguments);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_buffer.insert(g_buffer.end(), ledsColors, ledsColors + size);
    return Ok();
}

extern "C" bool CORSAIR_CALL CorsairSetLedsColorsFlushBuffer() {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<CorsairLedColor> pending;
    pending.swap(g_buffer);
    return ApplyLocked(static_cast<int>(pending.size()), pending.data());
}

extern "C" bool CORSAIR_CALL CorsairSetLedsColorsFlushBufferAsync(AsyncCallback callback,
                                                                  void* context) {
    bool r = CorsairSetLedsColorsFlushBuffer();
    CompleteAsync(callback, context, r);
    return r;
}

extern "C" bool CORSAIR_CALL CorsairGetLedsColors(int size, CorsairLedColor* ledsColors) {
    Bootstrap();
    if (size < 0 || (size > 0 && !ledsColors)) return Fail(CE_InvalidArguments);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < size; ++i) {
        Rgb c;
        if (!g_frame.Get(ledsColors[i].ledId, &c)) c = Rgb{};
        ledsColors[i].r = c.r;
        ledsColors[i].g = c.g;
        ledsColors[i].b = c.b;
    }
    return Ok();
}

extern "C" bool CORSAIR_CALL CorsairGetLedsColorsByDeviceIndex(int deviceIndex, int size,
                                                               CorsairLedColor* ledsColors) {
    if (!ValidDevice(deviceIndex)) return Fail(CE_InvalidArguments);
    return CorsairGetLedsColors(size, ledsColors);
}

extern "C" bool CORSAIR_CALL CorsairRequestControl(int /*accessMode*/) {
    Bootstrap();
    return Ok();
}

// Games that call this are done with lighting: hand Aura back to Armoury Crate. Games that
// never call it keep the mirror until they exit (the SDK has no shutdown call).
extern "C" bool CORSAIR_CALL CorsairReleaseControl(int /*accessMode*/) {
    Bootstrap();
    std::lock_guard<std::mutex> lock(g_mutex);
    LUMA_INFO("Corsair: ReleaseControl");
    g_mirror.Stop();
    g_handshakeDone = false;
    g_haveAmbient = false;
    return Ok();
}

extern "C" bool CORSAIR_CALL CorsairSetLayerPriority(int /*priority*/) { return Ok(); }

// Virtual devices have no keys to press and no hot-plug events.
extern "C" bool CORSAIR_CALL CorsairRegisterKeypressCallback(void* /*callback*/, void* /*context*/) {
    return Ok();
}
extern "C" bool CORSAIR_CALL CorsairSubscribeForEvents(void* /*callback*/, void* /*context*/) {
    return Ok();
}
extern "C" bool CORSAIR_CALL CorsairUnsubscribeFromEvents() { return Ok(); }

// No device properties (headset mic state, battery, ...) on virtual devices.
extern "C" bool CORSAIR_CALL CorsairGetBoolPropertyValue(int /*deviceIndex*/, int /*propertyId*/,
                                                         bool* /*value*/) {
    return Fail(CE_InvalidArguments);
}
extern "C" bool CORSAIR_CALL CorsairGetInt32PropertyValue(int /*deviceIndex*/, int /*propertyId*/,
                                                          int* /*value*/) {
    return Fail(CE_InvalidArguments);
}
