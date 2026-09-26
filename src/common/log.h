// Minimal thread-safe file logger. The proxy lives inside a game process with no console,
// so a log file is the only way to see what it is doing.
#pragma once

#include <string>

namespace luma::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

// Safe to call multiple times; later calls replace the target file/level.
// An empty path disables file output.
void Init(const std::wstring& path, Level level);
void Shutdown();

// Also echo log lines to stderr (for the command-line tools).
void SetConsoleEcho(bool enabled);

Level ParseLevel(const std::wstring& s, Level fallback);

void Write(Level level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

}  // namespace luma::log

#define LUMA_DEBUG(...) ::luma::log::Write(::luma::log::Level::Debug, __VA_ARGS__)
#define LUMA_INFO(...) ::luma::log::Write(::luma::log::Level::Info, __VA_ARGS__)
#define LUMA_WARN(...) ::luma::log::Write(::luma::log::Level::Warn, __VA_ARGS__)
#define LUMA_ERROR(...) ::luma::log::Write(::luma::log::Level::Error, __VA_ARGS__)
