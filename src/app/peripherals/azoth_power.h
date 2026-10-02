// Azoth power commands from G-Helper's AsusKeyboard.SetEnergySettings/ReadBattery.
// The firmware timer is independent of RGB writes. Pause it while LumaBridge owns sleep,
// then restore the observed ASUS timer when idle or released; never send a flash save.
#pragma once

#include <optional>
#include "azoth_protocol.h"

namespace luma::app::azoth {

constexpr uint8_t kNeverSleep = 0xFF;
inline bool ValidIdleTimeout(uint8_t value) { return value <= 4 || value == kNeverSleep; }

inline Report SetIdleTimeout(Link link, uint8_t value) {
    Report r{};
    r[0] = link == Link::Wired ? 0 : 2;
    r[1] = 0x51;
    r[2] = 0x38;
    r[5] = value;
    return r;
}

class IdleTimeoutOverride {
public:
    void Observe(uint8_t value) {
        if (!ValidIdleTimeout(value)) return;
        // If ASUS changes the timer during control, remember its new choice for hand-back.
        if (!overriding_ || value != kNeverSleep) original_ = value;
        observed_ = value;
    }
    std::optional<uint8_t> Desired(bool control) const {
        if (!original_) return std::nullopt;  // don't overwrite a timer we can't restore
        const auto target = control ? kNeverSleep : *original_;
        return observed_ == target ? std::nullopt : std::optional<uint8_t>(target);
    }
    void Applied(uint8_t value, bool control) {
        observed_ = value;
        overriding_ = control;
    }
private:
    std::optional<uint8_t> original_, observed_;
    bool overriding_ = false;
};

}  // namespace luma::app::azoth
