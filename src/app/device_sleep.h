// Devices going to sleep: after a while without being used, a device's lighting fades out and
// LumaBridge stops talking to it, so a wireless mouse or keyboard can sleep (constant frames
// would keep it awake and drain its battery). The next time it's used, it lights up again.
// Pure, tested.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace luma::app::sleep {

constexpr uint64_t kFadeMs = 2500;   // the fade out
constexpr uint64_t kQuietMs = 1500;  // after the fade: time for the last (dark) frame to go out

// Game SDKs often send changing colors as Static frames. The exemption follows the active
// game lighting feed, rather than the effect kind (which also includes desktop animations).
inline uint64_t Timeout(bool enabled, int seconds, bool keepAwakeForGame, bool gameLighting) {
    if (!enabled || (keepAwakeForGame && gameLighting)) return 0;
    return static_cast<uint64_t>(seconds < 10 ? 10 : seconds) * 1000;
}

// Brightness factor 0..1 at `now` for a device last used at `lastInput`: 1 until `timeoutMs`,
// then fading to 0 over kFadeMs. `timeoutMs` 0: never sleeps.
inline double Level(uint64_t now, uint64_t lastInput, uint64_t timeoutMs) {
    if (!timeoutMs || now <= lastInput) return 1.0;
    const uint64_t idle = now - lastInput;
    if (idle <= timeoutMs) return 1.0;
    if (idle >= timeoutMs + kFadeMs) return 0.0;
    return 1.0 - static_cast<double>(idle - timeoutMs) / kFadeMs;
}

// Asleep: faded out and the dark frame sent, so nothing more needs to go to the device.
inline bool Asleep(uint64_t now, uint64_t lastInput, uint64_t timeoutMs) {
    return timeoutMs && now > lastInput && now - lastInput >= timeoutMs + kFadeMs + kQuietMs;
}

// A worker can miss the fade entirely during a slow USB call. Always send a dark frame
// once before becoming quiet, and don't retry discovery until the device is used again.
class OutputSleep {
public:
    enum class Action { Awake, Dark, Quiet };
    Action Update(bool quiet) {
        const bool entered = quiet && !quiet_;
        quiet_ = quiet;
        return entered ? Action::Dark : quiet ? Action::Quiet : Action::Awake;
    }
private:
    bool quiet_ = false;
};

// G HUB's answer to GET /lighting/turn_off_for_inactivity, e.g.
//   {..."path": "/lighting/turn_off_for_inactivity", ... "payload": {..., "enabled": true}}
// -> its setting; nullopt if the answer doesn't say.
inline std::optional<bool> ParseGHubEnabled(const std::string& json) {
    if (json.find("turn_off_for_inactivity") == std::string::npos || json.find("SUCCESS") == std::string::npos)
        return std::nullopt;
    const size_t key = json.find("\"enabled\"");
    if (key == std::string::npos) return std::nullopt;
    size_t v = json.find(':', key);
    if (v == std::string::npos) return std::nullopt;
    ++v;
    while (v < json.size() && (json[v] == ' ' || json[v] == '\n' || json[v] == '\r' || json[v] == '\t')) ++v;
    if (json.compare(v, 4, "true") == 0) return true;
    if (json.compare(v, 5, "false") == 0) return false;
    return std::nullopt;
}

}  // namespace luma::app::sleep
