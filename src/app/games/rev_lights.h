// Rev lights shared by the racing games' telemetry feeds: calm blue at low revs, then green,
// yellow and red as the engine climbs, and a fast red flash once it's at the limit.
#pragma once

#include "effects.h"

namespace luma::app::games {

inline Rgb RevBlend(Rgb a, Rgb b, double t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return Rgb{static_cast<uint8_t>(a.r + (b.r - a.r) * t), static_cast<uint8_t>(a.g + (b.g - a.g) * t),
               static_cast<uint8_t>(a.b + (b.b - a.b) * t)};
}

// `revs` 0..1 (idle to redline); `limiter`: the game says the limiter or a shift light is on.
inline fx::Params RevLights(double revs, bool limiter) {
    fx::Params p;
    p.speed = 0;
    if (limiter || revs >= 0.95) {
        p.kind = fx::Kind::Strobe;
        p.color1 = Rgb{255, 0, 0};
        p.speed = 12;
        return p;
    }
    p.kind = fx::Kind::Static;
    const double r = revs;
    if (r < 0.5) p.color1 = Rgb{0, 90, 255};
    else if (r < 0.7) p.color1 = RevBlend(Rgb{0, 90, 255}, Rgb{0, 255, 60}, (r - 0.5) / 0.2);
    else if (r < 0.85) p.color1 = RevBlend(Rgb{0, 255, 60}, Rgb{255, 220, 0}, (r - 0.7) / 0.15);
    else p.color1 = RevBlend(Rgb{255, 220, 0}, Rgb{255, 0, 0}, (r - 0.85) / 0.1);
    return p;
}

inline double Clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

}  // namespace luma::app::games
