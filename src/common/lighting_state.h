// Tracks the "ambient" color a game has asked for, including the timed effects the
// LogiLed SDK animates on-device (flash / pulse), which we have to re-create ourselves
// for Aura. Pure logic with an injected clock so it is unit testable; not thread-safe
// (the proxy guards it with a mutex).
#pragma once

#include <cmath>
#include <cstdint>

#include "color.h"

namespace luma {

class LightingState {
public:
    enum class Effect { None, Flash, Pulse, Spectrum };

    // LOGI_LED_DURATION_INFINITE
    static constexpr int kInfinite = 0;

    void SetStatic(Rgb c) {
        base_ = c;
        effect_ = Effect::None;
        ++version_;
    }

    // Flash: color on for intervalMs, off for intervalMs, repeating for durationMs
    // (0 = until StopEffects / another set call).
    void StartFlash(Rgb c, int durationMs, int intervalMs, uint64_t nowMs) {
        StartEffect(Effect::Flash, c, durationMs, intervalMs, nowMs);
    }

    // Pulse: smooth fade color -> off -> color with a period of intervalMs.
    void StartPulse(Rgb c, int durationMs, int intervalMs, uint64_t nowMs) {
        StartEffect(Effect::Pulse, c, durationMs, intervalMs, nowMs);
    }

    // Spectrum: cycles through every hue once per periodMs, until StopEffects / a set call.
    void StartSpectrum(int periodMs, uint64_t nowMs) {
        StartEffect(Effect::Spectrum, Rgb{}, kInfinite, periodMs, nowMs);
    }

    void StopEffects() {
        if (effect_ != Effect::None) {
            effect_ = Effect::None;
            ++version_;
        }
    }

    void Save() {
        saved_ = base_;
        hasSaved_ = true;
    }

    void Restore() {
        if (hasSaved_) SetStatic(saved_);
    }

    bool IsAnimating(uint64_t nowMs) const { return ActiveEffect(nowMs) != Effect::None; }

    Rgb Evaluate(uint64_t nowMs) const {
        switch (ActiveEffect(nowMs)) {
        case Effect::Flash: {
            const uint64_t t = nowMs - effectStart_;
            const bool on = ((t / static_cast<uint64_t>(interval_)) % 2) == 0;
            return on ? effectColor_ : Rgb{};
        }
        case Effect::Pulse: {
            const double phase =
                static_cast<double>((nowMs - effectStart_) % static_cast<uint64_t>(interval_)) /
                interval_;
            const double k = 0.5 + 0.5 * std::cos(phase * 2.0 * 3.14159265358979323846);
            return Scale(effectColor_, k);
        }
        case Effect::Spectrum: {
            const double phase =
                static_cast<double>((nowMs - effectStart_) % static_cast<uint64_t>(interval_)) /
                interval_;
            return FromHue(phase * 360.0);
        }
        case Effect::None:
        default:
            return base_;
        }
    }

    // Bumped on every state change so a consumer can cheaply detect "nothing new".
    uint64_t Version() const { return version_; }

private:
    void StartEffect(Effect e, Rgb c, int durationMs, int intervalMs, uint64_t nowMs) {
        effect_ = e;
        effectColor_ = c;
        effectStart_ = nowMs;
        duration_ = durationMs < 0 ? 0 : durationMs;
        interval_ = intervalMs > 0 ? intervalMs : 500;
        ++version_;
    }

    Effect ActiveEffect(uint64_t nowMs) const {
        if (effect_ == Effect::None) return Effect::None;
        if (duration_ != kInfinite && nowMs - effectStart_ >= static_cast<uint64_t>(duration_))
            return Effect::None;  // expired: fall back to base color
        return effect_;
    }

    Rgb base_{};
    Rgb saved_{};
    bool hasSaved_ = false;

    Effect effect_ = Effect::None;
    Rgb effectColor_{};
    uint64_t effectStart_ = 0;
    int duration_ = 0;
    int interval_ = 500;

    uint64_t version_ = 0;
};

}  // namespace luma
