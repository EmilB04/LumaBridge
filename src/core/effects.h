// Per-LED lighting effects for addressable hardware (ARGB fans / strips, board LEDs).
// Pure and deterministic (time and LED position in, color out), so the app can preview
// exactly what the hardware shows, and everything is unit tested.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "color.h"

namespace luma::fx {

enum class Kind {
    Static,       // color1
    Breathing,    // color1 fading in and out
    Strobe,       // color1 on / off
    ColorCycle,   // every LED the same hue, cycling through the spectrum
    RainbowWave,  // hue spread along the LEDs, rotating
    Gradient,     // color1 -> color2 -> color1 around the LEDs, optionally flowing
    Comet,        // color1 head with a fading tail over color2, chasing around
    Twinkle,      // color1 with random sparkles of color2
    Beat,         // color1 flashes over color2, speeding up from `speed` to `speedEnd` beats a
                  // second over `rampSeconds` (a bomb's beeps); games only
};

struct Params {
    Kind kind = Kind::Static;
    Rgb color1{0, 140, 255};
    Rgb color2{255, 255, 255};
    double speed = 0.5;  // cycles per second (0 = frozen for the moving effects)
    // Rainbow (color cycle / rainbow wave): which hues, how vivid, how many around a fan.
    double hueStart = 0;     // degrees
    double hueSpan = 360;    // degrees; under 360 the hues go there and back, so rings stay seamless
    double saturation = 1;   // 0 = white .. 1 = full color
    double spread = 1;       // rainbow wave: rainbows around each ring
    bool reverse = false;    // moving patterns turn the other way
    // Beat: the tempo it reaches (beats a second) and how long it takes to get there.
    double speedEnd = 0, rampSeconds = 0;
    // When the effect started (GetTickCount64 ms), the same for every device so they're all in
    // step; 0: each device counts from when it got the effect.
    uint64_t epoch = 0;
};

// Seconds into the effect at `nowMs`: from its shared start, else from `sinceMs`.
inline double Seconds(const Params& p, uint64_t nowMs, uint64_t sinceMs) {
    const uint64_t from = p.epoch ? p.epoch : sinceMs;
    return nowMs > from ? static_cast<double>(nowMs - from) / 1000.0 : 0.0;
}

inline bool SameParams(const Params& a, const Params& b) {
    return a.kind == b.kind && a.color1 == b.color1 && a.color2 == b.color2 && a.speed == b.speed &&
           a.hueStart == b.hueStart && a.hueSpan == b.hueSpan && a.saturation == b.saturation && a.spread == b.spread &&
           a.reverse == b.reverse && a.speedEnd == b.speedEnd && a.rampSeconds == b.rampSeconds && a.epoch == b.epoch;
}

// Does the effect change over time? (Static ones only need re-sending, not re-rendering.)
inline bool IsAnimated(const Params& p) {
    return p.kind != Kind::Static && !(p.kind == Kind::Gradient && p.speed <= 0);
}

// Whether color2 is used (the UI shows a second color picker for these).
inline bool UsesSecondColor(Kind k) { return k == Kind::Gradient || k == Kind::Comet || k == Kind::Twinkle; }
// Whether the effect differs from LED to LED.
inline bool IsPerLed(Kind k) {
    return k == Kind::RainbowWave || k == Kind::Gradient || k == Kind::Comet || k == Kind::Twinkle;
}

namespace detail {
constexpr double kPi = 3.14159265358979323846;

inline double Frac(double x) { return x - std::floor(x); }

inline Rgb Lerp(Rgb a, Rgb b, double t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return Rgb{ClampByte(a.r + (b.r - a.r) * t), ClampByte(a.g + (b.g - a.g) * t), ClampByte(a.b + (b.b - a.b) * t)};
}

// A color of the (customized) rainbow at `x` (0..1, wraps).
inline Rgb RainbowAt(const Params& p, double x) {
    x = Frac(x);
    const double span = p.hueSpan < 1 ? 1 : p.hueSpan > 360 ? 360 : p.hueSpan;
    const double hue = span >= 359.5 ? p.hueStart + x * 360.0 : p.hueStart + (1.0 - std::fabs(2.0 * x - 1.0)) * span;
    const double sat = p.saturation < 0 ? 0 : p.saturation > 1 ? 1 : p.saturation;
    return Lerp(Rgb{255, 255, 255}, FromHue(hue), sat);
}

// Stable pseudo-random value in [0, 1) per LED (same LED -> same value every frame).
inline double Hash01(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return (x & 0xFFFFFF) / static_cast<double>(0x1000000);
}
}  // namespace detail

// Color of LED `i` of a ring / strip of `count` LEDs at time `t` seconds.
// Beats so far at `t` for Kind::Beat: the tempo rises from speed to speedEnd along t^2 over
// rampSeconds (slow at first, faster and faster), then stays. Continuous, so a rising tempo
// never restarts a beat.
inline double BeatPhase(const Params& p, double t) {
    const double s0 = p.speed, s1 = p.speedEnd > 0 ? p.speedEnd : p.speed, T = p.rampSeconds;
    if (t <= 0) return 0;
    if (T <= 0) return s0 * t;
    if (t <= T) return s0 * t + (s1 - s0) * t * t * t / (3 * T * T);
    return s0 * T + (s1 - s0) * T / 3 + s1 * (t - T);
}

inline Rgb Render(const Params& p, double t, int i, int count) {
    using namespace detail;
    if (count < 1) count = 1;
    // 0..1 around the ring; mirrored when reversed, so moving patterns turn the other way.
    // Unreversed, every moving pattern travels towards higher LED numbers (to the right).
    const double pos = static_cast<double>(p.reverse && i % count ? count - i % count : i % count) / count;
    const double phase = t * p.speed;
    switch (p.kind) {
    case Kind::Breathing:
        return Scale(p.color1, 0.5 + 0.5 * std::cos(phase * 2 * kPi));
    case Kind::Strobe:
        return Frac(phase) < 0.5 ? p.color1 : Rgb{};
    case Kind::ColorCycle:
        return RainbowAt(p, phase);
    case Kind::RainbowWave:
        return RainbowAt(p, pos * (p.spread > 0.1 ? p.spread : 0.1) - phase);
    case Kind::Gradient: {
        // Triangle wave so the ring is seamless: color1 at 0, color2 at 0.5, color1 at 1.
        const double x = Frac(pos - phase);
        return Lerp(p.color1, p.color2, 1.0 - std::fabs(2.0 * x - 1.0));
    }
    case Kind::Comet: {
        const double head = Frac(phase);
        const double behind = Frac(head - pos);  // 0 = at the head, growing along the tail
        const double tail = 0.35;
        const double k = behind < tail ? 1.0 - behind / tail : 0.0;
        return Lerp(p.color2, p.color1, k);
    }
    case Kind::Twinkle: {
        // Each LED sparkles once per cycle at its own random moment, fading out quickly.
        const double since = Frac(phase + Hash01(static_cast<uint32_t>(i) * 2654435761u));
        const double spark = since < 0.15 ? 1.0 - since / 0.15 : 0.0;
        return Lerp(p.color1, p.color2, spark);
    }
    case Kind::Beat: {
        // Each beat: a flash of color1 that fades back to color2 within 30 % of the beat.
        const double f = Frac(BeatPhase(p, t));
        return Lerp(p.color2, p.color1, f < 0.3 ? 1.0 - f / 0.3 : 0.0);
    }
    case Kind::Static:
    default:
        return p.color1;
    }
}

// How LEDs on an ARGB header are grouped into fans. Fans on a hub are chained, so fan 1
// is LEDs 0..ledsPerFan-1, fan 2 the next ledsPerFan, and so on. The default matches
// be quiet! Light Wings 120 mm (two rings, 20 LEDs per fan).
struct FanLayout {
    int fans = 3;
    int ledsPerFan = 20;
    bool repeatPerFan = true;  // every fan shows the whole pattern; else it spans all fans

    int Fans() const { return fans < 1 ? 1 : fans > 16 ? 16 : fans; }
    int LedsPerFan() const { return ledsPerFan < 1 ? 1 : ledsPerFan > 120 ? 120 : ledsPerFan; }
    int TotalLeds() const { return Fans() * LedsPerFan(); }
    bool operator==(const FanLayout& o) const {
        return Fans() == o.Fans() && LedsPerFan() == o.LedsPerFan() && repeatPerFan == o.repeatPerFan;
    }
    bool operator!=(const FanLayout& o) const { return !(*this == o); }
};

// A frame for a strip of `count` LEDs (the board's LEDs: one ring).
inline void RenderStrip(const Params& p, double t, int count, std::vector<Rgb>* out) {
    out->resize(count < 0 ? 0 : static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) (*out)[i] = Render(p, t, i, count);
}

// A frame for an ARGB header laid out as `layout` fans. `count` is how many LEDs are sent
// (-1 = exactly the layout); LEDs past the layout continue the pattern, so an extra fan
// the layout doesn't know about still lights up.
inline void RenderFans(const Params& p, double t, const FanLayout& layout, std::vector<Rgb>* out, int count = -1) {
    const int total = layout.TotalLeds();
    const int per = layout.LedsPerFan();
    if (count < 0) count = total;
    out->resize(static_cast<size_t>(count));
    // Twinkle has no shape to repeat: copying it to every fan would make all fans sparkle
    // on the same LEDs at once, so it always runs across all of them.
    const bool repeat = layout.repeatPerFan && p.kind != Kind::Twinkle;
    for (int i = 0; i < count; ++i)
        (*out)[i] = repeat ? Render(p, t, i % per, per) : Render(p, t, i % total, total);
}

// Test pattern for checking the layout: each fan its own color with its first LED white,
// and everything past the layout off. With the right LED count every fan is one solid
// color with a single white LED.
inline void RenderFanTest(const FanLayout& layout, std::vector<Rgb>* out, int count = -1) {
    static const Rgb kColors[] = {{255, 0, 0}, {0, 255, 0}, {0, 80, 255}, {255, 200, 0},
                                  {255, 0, 255}, {0, 255, 255}, {255, 110, 0}, {140, 0, 255}};
    const int total = layout.TotalLeds();
    const int per = layout.LedsPerFan();
    if (count < 0) count = total;
    out->assign(static_cast<size_t>(count), Rgb{});
    for (int i = 0; i < count && i < total; ++i)
        (*out)[i] = i % per == 0 ? Rgb{255, 255, 255} : kColors[(i / per) % 8];
}

}  // namespace luma::fx
