// Stand-in for a game: loads a LogiLed DLL (the LumaBridge proxy or the real Logitech one)
// and drives it through the calls BF1-style titles make. Lets you test milestones 1 and 3
// without launching a game.
//
//   logiled-harness <path-to-dll> [seconds-per-step]
//   logiled-harness --zones [path-to-dll] [seconds-per-step]
//
// --zones paints the mouse's lighting zones through LogiLedSetLightingForTargetZone, to find
// out how many zones of a mouse (G502 X Plus) G HUB lets an app color separately. Without a
// path it uses G HUB's own DLL.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>

#include "../../src/integrations/logitech/logiled_api.h"

namespace {

#define LUMA_HARNESS_PTR(ret, name, params, args) ret(LOGILED_CALL* p##name) params = nullptr;
LOGILED_ALL(LUMA_HARNESS_PTR)
#undef LUMA_HARNESS_PTR

DWORD g_stepMs = 2000;

void Step(const char* what) {
    std::printf("  %s\n", what);
    Sleep(g_stepMs);
}

// Zone colors: red, green, blue, yellow, magenta, cyan, white, orange (percentages).
constexpr int kZoneColors[][3] = {{100, 0, 0},   {0, 100, 0},     {0, 0, 100},     {100, 100, 0},
                                  {100, 0, 100}, {0, 100, 100}, {100, 100, 100}, {100, 50, 0}};
constexpr const char* kZoneNames[] = {"red", "green", "blue", "yellow", "magenta", "cyan", "white", "orange"};
constexpr int kZones = 8;
constexpr int kMouse = 3;  // LogiLed::DeviceType::Mouse

void ZonesAtOnce() {
    for (int z = 0; z < kZones; ++z) {
        const bool ok = pLogiLedSetLightingForTargetZone(kMouse, z, kZoneColors[z][0], kZoneColors[z][1], kZoneColors[z][2]);
        if (ok) std::printf("  zone %d -> %s\n", z, kZoneNames[z]);
    }
    Step("(holding)");
    Step("(holding)");
}

int ZoneTest() {
    std::printf("\nZone test (LogiLedSetLightingForTargetZone, device type mouse):\n");
    if (!pLogiLedSetLightingForTargetZone) {
        std::printf("  this DLL has no LogiLedSetLightingForTargetZone\n");
        return 1;
    }
    std::printf("A) Control: the whole mouse through LogiLedSetLighting.\n");
    pLogiLedSetLighting(100, 0, 0);
    Step("whole mouse red?");
    pLogiLedSetLighting(0, 0, 0);
    Step("whole mouse off?");

    std::printf("B) Every zone its own color at once (target: all devices).\n");
    bool accepted[kZones] = {};
    for (int z = 0; z < kZones; ++z) {
        accepted[z] = pLogiLedSetLightingForTargetZone(kMouse, z, 0, 0, 0);
        std::printf("  zone %d %s\n", z, accepted[z] ? "accepted" : "refused");
    }
    ZonesAtOnce();

    std::printf("C) One zone at a time, white, the rest off.\n");
    for (int z = 0; z < kZones; ++z) {
        if (!accepted[z]) continue;
        for (int o = 0; o < kZones; ++o)
            if (accepted[o]) pLogiLedSetLightingForTargetZone(kMouse, o, 0, 0, 0);
        pLogiLedSetLightingForTargetZone(kMouse, z, 100, 100, 100);
        char what[32];
        std::snprintf(what, sizeof what, "zone %d white", z);
        Step(what);
    }

    std::printf("D) B again, over a lit mouse (whole mouse blue first).\n");
    pLogiLedSetLighting(0, 0, 100);
    Step("whole mouse blue?");
    ZonesAtOnce();

    if (pLogiLedSetTargetDevice) {
        std::printf("E) B again, targeting RGB devices only.\n");
        pLogiLedSetTargetDevice(2 /* LOGI_DEVICETYPE_RGB */);
        Sleep(300);
        pLogiLedSetLighting(0, 0, 0);
        ZonesAtOnce();
    }
    std::printf("Done. Please report what the mouse showed at each step (A to E).\n");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: logiled-harness <path-to-dll> [seconds-per-step]\n"
                              L"       logiled-harness --zones [path-to-dll] [seconds-per-step]\n");
        return 2;
    }
    const bool zones = std::wcscmp(argv[1], L"--zones") == 0;
    wchar_t ghub[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\LGHUB\\sdks\\sdk_legacy_led_x64.dll", ghub, MAX_PATH);
    int arg = zones ? 2 : 1;
    const wchar_t* dll = ghub;
    if (arg < argc && !(zones && iswdigit(argv[arg][0]))) dll = argv[arg++];
    if (arg < argc) g_stepMs = static_cast<DWORD>(_wtoi(argv[arg])) * 1000;

    HMODULE m = LoadLibraryW(dll);
    if (!m) {
        std::fwprintf(stderr, L"LoadLibrary(%ls) failed: %lu\n", dll, GetLastError());
        return 1;
    }
    std::wprintf(L"Loaded %ls\n", dll);

    int missing = 0;
#define LUMA_HARNESS_RESOLVE(ret, name, params, args)                                   \
    p##name = reinterpret_cast<decltype(p##name)>(                                      \
        reinterpret_cast<void*>(GetProcAddress(m, #name)));                             \
    if (!p##name) {                                                                     \
        std::printf("missing export: %s\n", #name);                                     \
        ++missing;                                                                      \
    }
    LOGILED_ALL(LUMA_HARNESS_RESOLVE)
#undef LUMA_HARNESS_RESOLVE
    std::printf("%d export(s) missing\n", missing);
    if (!pLogiLedInit || !pLogiLedSetLighting || !pLogiLedShutdown) return 1;

    bool ok = pLogiLedInitWithName ? pLogiLedInitWithName("LumaBridge harness") : pLogiLedInit();
    std::printf("LogiLedInit -> %s\n", ok ? "true" : "false");

    int major = 0, minor = 0, build = 0;
    if (pLogiLedGetSdkVersion && pLogiLedGetSdkVersion(&major, &minor, &build))
        std::printf("SDK version %d.%d.%d\n", major, minor, build);

    Sleep(500);  // G HUB needs a moment after init before it accepts colors
    if (pLogiLedSetTargetDevice) pLogiLedSetTargetDevice(7 /* LOGI_DEVICETYPE_ALL */);

    if (zones) {
        const int r = ZoneTest();
        pLogiLedShutdown();
        std::printf("LogiLedShutdown done\n");
        FreeLibrary(m);
        return r;
    }

    std::printf("Static colors (LogiLedSetLighting):\n");
    pLogiLedSetLighting(100, 0, 0);
    Step("red");
    pLogiLedSetLighting(0, 100, 0);
    Step("green");
    pLogiLedSetLighting(0, 0, 100);
    Step("blue");

    if (pLogiLedSaveCurrentLighting && pLogiLedRestoreLighting) {
        std::printf("Save / restore:\n");
        pLogiLedSaveCurrentLighting();
        pLogiLedSetLighting(100, 100, 100);
        Step("white (saved blue)");
        pLogiLedRestoreLighting();
        Step("restored -> blue");
    }

    if (pLogiLedSetLightingFromBitmap) {
        std::printf("Bitmap (orange left half, black right half):\n");
        unsigned char bmp[LOGI_LED_BITMAP_SIZE] = {};
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 10; ++x) {
                unsigned char* p = bmp + (y * 21 + x) * 4;
                p[0] = 0;    // B
                p[1] = 128;  // G
                p[2] = 255;  // R
                p[3] = 255;
            }
        pLogiLedSetLightingFromBitmap(bmp);
        Step("orange (ambient = average of lit keys)");
    }

    if (pLogiLedFlashLighting) {
        std::printf("Flash / pulse:\n");
        pLogiLedFlashLighting(100, 0, 100, static_cast<int>(g_stepMs), 200);
        Step("flashing magenta");
        Sleep(300);
    }
    if (pLogiLedPulseLighting && pLogiLedStopEffects) {
        pLogiLedPulseLighting(0, 100, 100, 0 /* infinite */, 1000);
        Step("pulsing cyan");
        pLogiLedStopEffects();
        Step("effects stopped");
    }

    if (pLogiLedSetLightingForKeyWithScanCode) {
        // Pass-through only for now (per-key mapping is milestone 4).
        pLogiLedSetLighting(0, 0, 0);
        pLogiLedSetLightingForKeyWithScanCode(0x11 /* W */, 100, 100, 0);
        Step("per-key: W yellow on Logitech keyboard (Aura unchanged: black)");
    }

    pLogiLedShutdown();
    std::printf("LogiLedShutdown done\n");
    FreeLibrary(m);
    return 0;
}
