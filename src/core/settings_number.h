// Strict, finite numeric settings. Invalid text preserves a documented fallback.
#pragma once
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <algorithm>
#include <limits>
#include <string>
namespace luma::settings {
inline bool Parse(const std::string& text, double* value) {
    if (text.empty()) return false;
    char* end = nullptr; errno = 0;
    const double n = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || errno == ERANGE || !std::isfinite(n)) return false;
    *value = n; return true;
}
inline bool Parse(const std::wstring& text, double* value) {
    if (text.empty()) return false;
    wchar_t* end = nullptr; errno = 0;
    const double n = std::wcstod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || errno == ERANGE || !std::isfinite(n)) return false;
    *value = n; return true;
}
template<class Text> inline double Number(const Text& text, double fallback = 0) {
    double n; return Parse(text, &n) ? n : fallback;
}
template<class Text> inline int Integer(const Text& text, int fallback = 0) {
    double n;
    return Parse(text, &n) && std::trunc(n) == n && n >= std::numeric_limits<int>::min() && n <= std::numeric_limits<int>::max()
        ? static_cast<int>(n) : fallback;
}
template<class Text, class T> inline T Clamped(const Text& text, T fallback, T lo, T hi) {
    double n;
    return Parse(text, &n) ? static_cast<T>(std::clamp(n, static_cast<double>(lo), static_cast<double>(hi))) : fallback;
}
}  // namespace luma::settings
