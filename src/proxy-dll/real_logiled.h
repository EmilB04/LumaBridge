// Loads the genuine Logitech LED SDK DLL and resolves its exports so the proxy can forward.
#pragma once

#include <string>

#include "logiled_api.h"

namespace luma::real {

#define LUMA_DECLARE_PTR(ret, name, params, args) \
    using name##_fn = ret(LOGILED_CALL*) params;  \
    extern name##_fn name;
LOGILED_ALL(LUMA_DECLARE_PTR)
#undef LUMA_DECLARE_PTR

// Resolves and loads the real DLL once. `configuredPath` (may be empty) wins over
// auto-detection. Never loads the proxy itself. Returns true if the DLL is loaded;
// individual pointers can still be null if that export is missing (old SDK versions).
bool Load(const std::wstring& configuredPath, const std::wstring& selfPath);

bool IsLoaded();
std::wstring LoadedPath();

}  // namespace luma::real
