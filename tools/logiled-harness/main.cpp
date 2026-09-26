// Stand-in for a game: loads a LogiLed DLL (the LumaBridge proxy or the real Logitech one)
// and drives it through the calls BF1-style titles make. Lets you test milestones 1 and 3
// without launching a game.
//
//   logiled-harness <path-to-dll> [seconds-per-step]
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "../../src/proxy-dll/logiled_api.h"

namespace {

#define LUMA_HARNESS_PTR(ret, name, params, args) ret(LOGILED_CALL* p##name) params = nullptr;
LOGILED_ALL(LUMA_HARNESS_PTR)
#undef LUMA_HARNESS_PTR

DWORD g_stepMs = 2000;

void Step(const char* what) {
    std::printf("  %s\n", what);
    Sleep(g_stepMs);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: logiled-harness <path-to-dll> [seconds-per-step]\n");
        return 2;
    }
    if (argc >= 3) g_stepMs = static_cast<DWORD>(_wtoi(argv[2])) * 1000;

    HMODULE m = LoadLibraryW(argv[1]);
    if (!m) {
        std::fwprintf(stderr, L"LoadLibrary(%ls) failed: %lu\n", argv[1], GetLastError());
        return 1;
    }

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
