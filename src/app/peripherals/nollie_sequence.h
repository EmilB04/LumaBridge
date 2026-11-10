// Sequences across a Nollie hub's fans and strips: effects that know the order of the parts
// (nollie::TravelSteps), such as filling fan 1 around its ring, then fan 2, and so on until
// every fan is lit, then starting over. A part is a fan on its own, or a whole channel (a strip,
// fans sharing one pattern). Pure and tested.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "color.h"
#include "nollie_layout.h"

namespace luma::app::nollie {

// A color gradient laid along the hub's parts in channel order (0 = the first LED of the
// first part, 1 = the last LED of the last), replacing the effect's main color LED by LED.
// `flow` (cycles a second) moves it along; a moving gradient loops back to its first color.
constexpr size_t kMaxGradientStops = 6;

struct Gradient {
    bool on = false;
    std::vector<Rgb> stops{Rgb{255, 0, 80}, Rgb{0, 120, 255}};
    double flow = 0;

    bool operator==(const Gradient& o) const { return on == o.on && stops == o.stops && flow == o.flow; }
    bool operator!=(const Gradient& o) const { return !(*this == o); }
};

inline Rgb GradientAt(const Gradient& g, double x, double t) {
    if (g.stops.empty()) return Rgb{};
    if (g.stops.size() == 1) return g.stops[0];
    const bool loop = g.flow != 0;
    x -= g.flow * t;
    x = loop ? x - std::floor(x) : (x < 0 ? 0 : x > 1 ? 1 : x);
    // A loop runs through every stop and back to the first; a still gradient from first to last.
    const size_t n = g.stops.size();
    const double spans = loop ? static_cast<double>(n) : static_cast<double>(n - 1);
    const double at = x * spans;
    size_t i = static_cast<size_t>(at);
    if (i >= static_cast<size_t>(spans)) i = static_cast<size_t>(spans) - 1;
    const double f = at - static_cast<double>(i);
    const Rgb a = g.stops[i % n], b = g.stops[(i + 1) % n];
    return Rgb{ClampByte(a.r + (b.r - a.r) * f), ClampByte(a.g + (b.g - a.g) * f), ClampByte(a.b + (b.b - a.b) * f)};
}

// Where along the hub a channel's LEDs sit, for the gradient: its first part is step `first`
// of `parts` (forward order, TravelSteps).
struct GradientPlace {
    const Gradient* gradient = nullptr;  // null or off: no gradient
    int first = 0, parts = 1;
    double t = 0;  // seconds, for a flowing gradient
    bool active() const { return gradient && gradient->on && !gradient->stops.empty(); }
    // The color at LED `j` of `length` in the channel's part `local`.
    Rgb At(int local, int j, int length) const {
        const double x = (first + local + (j + 0.5) / length) / (parts < 1 ? 1 : parts);
        return GradientAt(*gradient, x, t);
    }
};

enum class Sequence {
    None,          // the hub shows its lighting (travelling or not)
    FillCircles,   // each part fills around its ring in turn; all lit for a moment; off; again
    FillAndEmpty,  // fills in turn, then empties in the same order
    OneAtATime,    // one part lit at a time, fading in and out, moving on
    RainbowStack,  // fills in turn, each part in its own color of the rainbow
    Bounce,        // a comet running through every LED of every part and back
    Stack,         // a dot runs to the end and stays; the next one stacks behind it
};
constexpr int kSequences = 7;

inline const char* SequenceName(Sequence s) {
    switch (s) {
    case Sequence::FillCircles: return "Fill each fan, one after another";
    case Sequence::FillAndEmpty: return "Fill, then empty";
    case Sequence::OneAtATime: return "One fan at a time";
    case Sequence::RainbowStack: return "Rainbow, fan by fan";
    case Sequence::Bounce: return "Bounce through all fans";
    case Sequence::Stack: return "Stack up";
    default: return "None";
    }
}

struct SequenceParams {
    Sequence kind = Sequence::None;
    Rgb color1{0, 140, 255};  // lit
    Rgb color2{0, 0, 0};      // unlit
    double partSeconds = 1.0; // time one part takes (a fan filling, a fan's turn)

    bool operator==(const SequenceParams& o) const {
        return kind == o.kind && color1 == o.color1 && color2 == o.color2 && partSeconds == o.partSeconds;
    }
    bool operator!=(const SequenceParams& o) const { return !(*this == o); }
};

namespace seqdetail {
inline double Frac(double x) { return x - std::floor(x); }
inline double Clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
inline Rgb Mix(Rgb a, Rgb b, double t) {
    t = Clamp01(t);
    return Rgb{ClampByte(a.r + (b.r - a.r) * t), ClampByte(a.g + (b.g - a.g) * t), ClampByte(a.b + (b.b - a.b) * t)};
}
// How much of an LED at `x` (0..1 around the ring, `width` one LED) is lit when the ring is
// filled to `fill` (0..1): the LED at the edge partly, so the fill moves smoothly.
inline double Filled(double fill, double x, double width) {
    if (width <= 0) width = 1e-6;
    return Clamp01((fill - x) / width);
}
}  // namespace seqdetail

// The color of the LED at `x` (0..1 along its part, `width` the share one LED takes) of part
// `part` of `parts`, `t` seconds into the sequence.
inline Rgb SequenceColor(const SequenceParams& s, double t, int part, int parts, double x, double width) {
    using namespace seqdetail;
    if (parts < 1) parts = 1;
    const double S = s.partSeconds > 0.05 ? s.partSeconds : 0.05;
    const double N = parts;
    if (t < 0) t = 0;
    switch (s.kind) {
    case Sequence::FillCircles:
    case Sequence::RainbowStack: {
        // Filling takes N parts' time, then all stay lit for one part's time, then off.
        const double tau = std::fmod(t, (N + 1) * S);
        const double fill = Clamp01((tau - part * S) / S);
        Rgb lit = s.color1;
        if (s.kind == Sequence::RainbowStack) lit = FromHue(360.0 * part / N);
        return Mix(s.color2, lit, Filled(fill, x, width));
    }
    case Sequence::FillAndEmpty: {
        const double tau = std::fmod(t, 2 * N * S);
        const double fill = tau < N * S ? Clamp01((tau - part * S) / S) : 1.0 - Clamp01((tau - N * S - part * S) / S);
        // Emptying takes the LEDs away from the start of the ring, the way they came.
        const double amount = tau < N * S ? Filled(fill, x, width) : 1.0 - Filled(1.0 - fill, x, width);
        return Mix(s.color2, s.color1, amount);
    }
    case Sequence::OneAtATime: {
        const double tau = std::fmod(t, N * S);
        const int active = std::min(parts - 1, static_cast<int>(tau / S));
        if (active != part) return s.color2;
        return Mix(s.color2, s.color1, std::sin(Frac(tau / S) * 3.14159265358979323846));
    }
    case Sequence::Bounce: {
        // There and back through every part in 2N parts' time; the tail a fifth of a part.
        const double u = Frac(t / (2 * N * S));
        const double head = u < 0.5 ? 2 * u : 2 - 2 * u;  // 0..1 along all parts
        const bool forward = u < 0.5;
        const double g = (part + x) / N;
        const double behind = forward ? head - g : g - head;
        const double tail = std::max(0.2 / N, width / N * 2);
        return Mix(s.color2, s.color1, behind >= 0 && behind < tail ? 1.0 - behind / tail : 0.0);
    }
    case Sequence::Stack: {
        // Four cells a part. A dot runs from the start to the last free cell and stays; the next
        // one stacks behind it, until every cell is lit; held for one part's time, then off.
        // A full stack takes three parts' time per part.
        const int cells = parts * 4;
        const double dt = 6.0 * N * S / (static_cast<double>(cells) * (cells + 1));  // one cell's step
        const double stacking = cells * (cells + 1) / 2.0 * dt;
        const double tau = std::fmod(t, stacking + S);
        const int cell = std::min(cells - 1, static_cast<int>((part + x + width / 2) / N * cells));
        double elapsed = tau;
        for (int k = 0; k < cells; ++k) {
            const int target = cells - 1 - k;  // where dot k comes to rest
            const double travel = (target + 1) * dt;
            if (elapsed < travel) {
                const int at = std::min(target, static_cast<int>(elapsed / dt));
                return cell == at || cell > target ? s.color1 : s.color2;
            }
            elapsed -= travel;
        }
        return s.color1;  // every dot in place
    }
    default:
        return s.color2;
    }
}

// A channel showing the sequence. `firstPart`: the step of the channel's first part along the
// hub (TravelSteps); `parts`: steps in all; `brightness[i]`: of the channel's part i (missing: 1);
// `reverse`: fill its rings the other way round; `backward`: its fans count down (the sequence
// goes from the last part to the first); `gradient`: lit in its colors instead of color1.
inline std::vector<Rgb> RenderSequence(const Channel& c, const Model& m, const SequenceParams& s, double t, int firstPart,
                                       int parts, const std::vector<double>& brightness, bool reverse,
                                       bool backward = false, const GradientPlace& gradient = {}) {
    std::vector<Rgb> out;
    const int n = c.Leds(m);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    const bool eachFan = c.Segments() > 1;
    const int per = c.LedsPerFan();
    for (int i = 0; i < n; ++i) {
        const int local = eachFan ? i / per : 0;
        const int j = eachFan ? i % per : i;
        const int length = eachFan ? per : n;
        double x = static_cast<double>(j) / length;
        if (reverse) x = 1.0 - static_cast<double>(j + 1) / length;
        const int step = firstPart + (backward ? c.Segments() - 1 - local : local);
        SequenceParams here = s;
        if (gradient.active()) here.color1 = gradient.At(local, j, length);  // lit in the gradient's color
        const Rgb color = SequenceColor(here, t, step, parts, x, 1.0 / length);
        const double b = local < static_cast<int>(brightness.size()) ? brightness[static_cast<size_t>(local)] : 1.0;
        out[static_cast<size_t>(i)] = b < 1.0 ? Scale(color, b < 0 ? 0 : b) : color;
    }
    return out;
}

// RenderSegments with the gradient: each LED's effect gets the gradient's color at its place
// as its main color (rainbow effects keep their own colors).
inline std::vector<Rgb> RenderSegments(const Channel& c, const Model& m, const std::vector<Segment>& segments,
                                       const GradientPlace& gradient) {
    if (!gradient.active()) return RenderSegments(c, m, segments);
    std::vector<Rgb> out;
    const int n = c.Leds(m);
    if (n <= 0 || segments.empty()) return out;
    out.assign(static_cast<size_t>(n), Rgb{});
    const bool eachFan = c.Segments() > 1;
    const int per = c.LedsPerFan();
    const int ring = c.kind == Kind::Strip ? n : c.repeatPerFan ? per : c.Fans() * per;
    for (int i = 0; i < n; ++i) {
        const size_t part = eachFan ? static_cast<size_t>(i / per) : 0;
        if (part >= segments.size()) break;
        const Segment& s = segments[part];
        const int j = eachFan ? i % per : i;
        const int length = eachFan ? per : n;
        const double t = s.t - s.lag * j / length;
        fx::Params p = s.effect;
        p.color1 = gradient.At(static_cast<int>(part), j, length);
        const Rgb color = p.kind == fx::Kind::Twinkle ? fx::Render(p, t, i, n) : fx::Render(p, t, i % ring, ring);
        out[static_cast<size_t>(i)] = s.brightness < 1.0 ? Scale(color, s.brightness < 0 ? 0 : s.brightness) : color;
    }
    return out;
}

}  // namespace luma::app::nollie
