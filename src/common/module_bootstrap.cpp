#include "module_bootstrap.h"

#include <iterator>

#include "log.h"

namespace luma {

std::string NarrowForLog(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s += (c < 128) ? static_cast<char>(c) : '?';
    return s;
}

Config BootstrapModule(HMODULE self, const wchar_t* logSuffix, const char* displayName) {
    wchar_t buf[MAX_PATH * 2];
    std::wstring selfPath;
    DWORD n = GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) selfPath.assign(buf, n);

    Config cfg = LoadConfig(selfPath.substr(0, selfPath.find_last_of(L"\\/")));
    if (!cfg.logFile.empty() && logSuffix && *logSuffix) {
        size_t dot = cfg.logFile.find_last_of(L'.');
        size_t sep = cfg.logFile.find_last_of(L"\\/");
        std::wstring stem = (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep))
                                ? cfg.logFile
                                : cfg.logFile.substr(0, dot);
        cfg.logFile = stem + L"-" + logSuffix + L".log";
    }
    log::Init(cfg.logFile, cfg.logLevel);

    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    LUMA_INFO("==== LumaBridge %s loaded into %s (pid %lu) ====", displayName,
              NarrowForLog(exe).c_str(), GetCurrentProcessId());
    LUMA_INFO("module: %s", NarrowForLog(selfPath).c_str());
    LUMA_INFO("config: %s", cfg.sourcePath.empty() ? "(none found, using defaults)"
                                                   : NarrowForLog(cfg.sourcePath).c_str());
    return cfg;
}

}  // namespace luma
