#include "log.h"

#include <windows.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace luma::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
Level g_level = Level::Info;
bool g_console = false;

const char* LevelName(Level l) {
    switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    default: return "?    ";
    }
}

}  // namespace

void Init(const std::wstring& path, Level level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_level = level;
    if (path.empty() || level == Level::Off) return;

    // Create the parent directory (one level is enough for %LOCALAPPDATA%\LumaBridge).
    size_t sep = path.find_last_of(L"\\/");
    if (sep != std::wstring::npos) CreateDirectoryW(path.substr(0, sep).c_str(), nullptr);

    g_file = _wfsopen(path.c_str(), L"a", _SH_DENYWR);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void SetConsoleEcho(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_console = enabled;
}

Level ParseLevel(const std::wstring& s, Level fallback) {
    if (_wcsicmp(s.c_str(), L"debug") == 0) return Level::Debug;
    if (_wcsicmp(s.c_str(), L"info") == 0) return Level::Info;
    if (_wcsicmp(s.c_str(), L"warn") == 0) return Level::Warn;
    if (_wcsicmp(s.c_str(), L"error") == 0) return Level::Error;
    if (_wcsicmp(s.c_str(), L"off") == 0) return Level::Off;
    return fallback;
}

void Write(Level level, const char* fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (level < g_level) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] %s %s\n", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             GetCurrentThreadId(), LevelName(level), msg);

    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
    if (g_console) fputs(line, stderr);
    OutputDebugStringA(line);  // visible in DebugView / an attached debugger
}

}  // namespace luma::log
