// Shared start-up for the SDK emulator DLLs (Chroma, Corsair, LightFX): find our own path,
// load LumaBridge.ini, open a per-emulator log file and write the banner.
#pragma once

#include <windows.h>

#include <string>

#include "config.h"

namespace luma {

// `logSuffix` turns lumabridge.log into lumabridge-<suffix>.log so several emulators loaded
// into one game don't interleave. Call once (callers wrap it in std::call_once).
Config BootstrapModule(HMODULE self, const wchar_t* logSuffix, const char* displayName);

// Plain ASCII narrowing for log lines (paths with non-ASCII characters print as '?').
std::string NarrowForLog(const std::wstring& w);

}  // namespace luma
