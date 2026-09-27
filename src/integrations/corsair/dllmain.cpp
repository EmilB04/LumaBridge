#include <windows.h>

namespace luma {
extern HMODULE g_selfModule;
}

// Deliberately does almost nothing: no LoadLibrary, no COM, no threads under the loader
// lock. All real initialisation is deferred to the first exported call (see exports.cpp).
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        luma::g_selfModule = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
