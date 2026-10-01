// Device settings use an encoded hardware identity, not a product name: several identical
// memory modules, mice or controllers must keep independent switches. Safe in the INI list.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <algorithm>
#include <cctype>

namespace luma::app::lighting {
inline std::string Identity(std::initializer_list<std::string> fields) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto& field : fields) {
        for (unsigned char c : field) { hash ^= c; hash *= 1099511628211ull; }
        hash ^= 0; hash *= 1099511628211ull;
    }
    std::string id(16, '0');
    for (int i = 15; i >= 0; --i) { id[i] = "0123456789abcdef"[hash & 15]; hash >>= 4; }
    return id;
}

// Read older name-based choices until a device gets its own saved choice.
inline bool Enabled(const std::map<std::string, bool>& choices, const std::string& id,
                    const std::string& name, bool fallback) {
    auto it = choices.find(id);
    if (it == choices.end()) it = choices.find(name);
    return it == choices.end() ? fallback : it->second;
}

struct NativeConnections {
    bool aura = false, logitech = false, azoth = false, memory = false, kraken = false, dualsense = false;
};

// Availability matters: selecting native control must not block a working fallback when
// the native driver cannot reach this hardware. No brand whitelist for the fallback.
inline bool PreferNative(int type, std::string who, NativeConnections available) {
    std::transform(who.begin(), who.end(), who.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto has = [&](const char* part) { return who.find(part) != std::string::npos; };
    return (available.aura && (type == 0 || type == 4) && has("asus")) ||
           (available.logitech && has("logitech")) || (available.azoth && has("azoth")) ||
           (available.memory && type == 1 && (has("kingston") || has("hyperx") || has("fury"))) ||
           (available.kraken && type == 3 && has("kraken")) ||
           (available.dualsense && type == 10 && has("dualsense"));
}

inline bool SameLocation(std::string description, std::string path) {
    if (path.size() < 12) return false;  // match an actual HID path, not a short vendor ID
    for (auto* text : {&description, &path})
        std::transform(text->begin(), text->end(), text->begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return description.find(path) != std::string::npos;
}

// An SDK connection covers all Logitech gear; a direct HID++ connection covers one
// known product. Do not disable other devices' fallback connections by brand alone.
inline bool LogitechCovered(bool sdk, std::string directName, std::string name) {
    if (sdk) return true;
    if (directName.empty()) return false;
    for (auto* value : {&directName, &name})
        std::transform(value->begin(), value->end(), value->begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name == directName;
}
}  // namespace luma::app::lighting
