// What a DualSense shows for an effect. The lightbar is one color and the five player LEDs
// are on or off, so moving effects need a reading of the effect for each:
//   - the lightbar takes the average color of the effect across the controller (a comet
//     pulses as it passes, a fill brightens as it fills, sparkles flicker), or the color the
//     effect has at its first LED where that is what moves (rainbows and flowing gradients
//     cycle their hue there);
//   - the player LEDs show the motion: a comet chases across them, a fill fills them from
//     one side, sparkles twinkle, a strobe blinks them all, breathing is a level meter.
// Still effects (a color, a color cycle) leave them off, as before. Pure and tested.
#pragma once

#include <algorithm>
#include <cstdint>

#include "color.h"
#include "effects.h"

namespace luma::app::dualsense {

// Preferences apply only to the five white indicators; the RGB lightbar keeps its effect.
static_assert(fx::kFirstSetupEffect + fx::kSetupEffects <= 32, "player effect mask fits every effect");
struct PlayerOptions {
    int mode = 0;  // follow effect, steady, chase, breathing meter, blink
    uint8_t mask = 0x1F;
    uint32_t effects = 0xFFFFFFFFu;
    double speed = 0.5;
};

struct Frame {
    Rgb bar;
    uint8_t players = 0;  // bit 0 .. 4: the five player LEDs, from the left
    bool operator==(const Frame& o) const { return bar == o.bar && players == o.players; }
    bool operator!=(const Frame& o) const { return !(*this == o); }
};

// How bright a color is: its strongest channel (a pure red is as bright as a white), 0..1.
inline double Lightness(Rgb c) { return std::max({c.r, c.g, c.b}) / 255.0; }

// Whether the effect's motion shows along the controller (so the lightbar averages it).
inline bool AveragesAcross(const fx::Params& p) {
    return p.kind == fx::Kind::Comet || p.kind == fx::Kind::Twinkle || fx::IsSetupEffect(p.kind);
}

// The five player LEDs for the effect: `color` is the unscaled color at the first LED.
inline uint8_t PlayerLeds(const fx::Params& p, double t, Rgb color) {
    constexpr double kOn = 0.30;  // how bright an LED of the effect must be for the player LED to light
    uint8_t mask = 0;
    switch (p.kind) {
    case fx::Kind::Comet:
    case fx::Kind::Twinkle:
    case fx::Kind::Gradient:
    case fx::Kind::RainbowWave:
        // Gradients and rainbows only when they move: a still one is a still color.
        if ((p.kind == fx::Kind::Gradient || p.kind == fx::Kind::RainbowWave) && (p.speed <= 0 || p.kind == fx::Kind::RainbowWave))
            return 0;
        for (int i = 0; i < 5; ++i)
            if (Lightness(fx::Render(p, t, i, 5)) > kOn) mask |= static_cast<uint8_t>(1 << i);
        return mask;
    case fx::Kind::Strobe:
    case fx::Kind::Beat:
        return Lightness(color) > kOn ? 0x1F : 0;  // all of them blink together
    case fx::Kind::Breathing: {
        // A level meter: the brighter the breath, the more LEDs, from the left.
        fx::Params full = p;
        full.kind = fx::Kind::Static;
        const double top = std::max(0.01, Lightness(fx::Render(full, 0, 0, 1)));
        const int n = static_cast<int>(std::clamp(Lightness(color) / top, 0.0, 1.0) * 5 + 0.999);
        return static_cast<uint8_t>((1 << std::clamp(n, 0, 5)) - 1);
    }
    default:
        if (fx::IsSetupEffect(p.kind)) {
            for (int i = 0; i < 5; ++i)
                if (Lightness(fx::Render(p, t, i, 5)) > kOn) mask |= static_cast<uint8_t>(1 << i);
            return mask;
        }
        return 0;  // a color, a color cycle: nothing moves
    }
}

// The lightbar and the player LEDs at `t` seconds, at `brightness` (0..1 on the lightbar; the
// player LEDs have no dimmer of their own, so they follow the effect).
inline Frame Compose(const fx::Params& p, double t, double brightness, bool playerLeds, const PlayerOptions& options = {}) {
    Frame f;
    const Rgb first = fx::Render(p, t, 0, 1);
    Rgb bar = first;
    if (AveragesAcross(p)) {
        constexpr int kSamples = 16;
        double r = 0, g = 0, b = 0;
        for (int i = 0; i < kSamples; ++i) {
            const Rgb c = fx::Render(p, t, i, kSamples);
            r += c.r;
            g += c.g;
            b += c.b;
        }
        bar = Rgb{ClampByte(r / kSamples), ClampByte(g / kSamples), ClampByte(b / kSamples)};
    }
    f.bar = Scale(bar, brightness);
    const int kind = static_cast<int>(p.kind);
    if (playerLeds && brightness > 0.02 && kind >= 0 && kind < 32 && (options.effects & (uint32_t{1} << kind))) {
        fx::Params indicators = p;
        indicators.color1 = Rgb{255, 255, 255};
        indicators.color2 = Rgb{};
        indicators.speed = std::clamp(options.speed, 0.1, 3.0);
        switch (options.mode) {
        case 1: f.players = 0x1F; break;
        case 2:
            indicators.kind = fx::Kind::Comet;
            f.players = PlayerLeds(indicators, t, fx::Render(indicators, t, 0, 1));
            break;
        case 3:
        case 4:
            indicators.kind = options.mode == 3 ? fx::Kind::Breathing : fx::Kind::Strobe;
            f.players = PlayerLeds(indicators, t, fx::Render(indicators, t, 0, 1));
            break;
        default: f.players = PlayerLeds(p, t, first); break;
        }
        f.players &= options.mask & 0x1F;
    }
    return f;
}

}  // namespace luma::app::dualsense
