// Keeps a service stopped without losing its original startup setting. The store must
// survive a helper restart; failed writes must leave the previous value intact.
#pragma once

#include <cstdint>

namespace luma::app::service {

enum class SavedStart { Missing, Found, Failed };
constexpr uint32_t kDisabled = 4;  // Windows SERVICE_DISABLED

template <typename Service, typename Store>
bool HoldStarts(Service& service, Store& store) {
    uint32_t original = 0;
    const auto saved = store.Read(&original);
    if (saved == SavedStart::Failed) return false;
    uint32_t current = 0;
    if (!service.Read(&current)) return false;
    if (current == kDisabled) return true;  // preserve a service the owner already disabled
    if (current != 2 && current != 3) return false;  // only automatic / manual Win32 services
    if (saved == SavedStart::Missing && !store.Write(current)) return false;
    // Keep the saved value even if this fails: a later helper can retry or restore it.
    return service.Write(kDisabled);
}

template <typename Service, typename Store>
bool RestoreStarts(Service& service, Store& store) {
    uint32_t original = 0;
    const auto saved = store.Read(&original);
    if (saved == SavedStart::Missing) return true;
    if (saved == SavedStart::Failed || !service.Write(original)) return false;
    return store.Clear();
}

}  // namespace luma::app::service
