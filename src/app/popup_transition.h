#pragma once

#include <algorithm>
#include <cmath>

namespace luma::app {

// Keeping the same progress on reversal avoids a flash when a hover returns mid-fade.
struct PopupTransition {
    float progress = 0;

    float Alpha() const { return progress * progress * (3.f - 2.f * progress); }

    float Advance(bool visible, float seconds) {
        if (std::isfinite(seconds) && seconds > 0)
            progress = std::clamp(progress + (visible ? seconds / 0.16f : -seconds / 0.12f), 0.f, 1.f);
        return Alpha();
    }
};

}  // namespace luma::app
