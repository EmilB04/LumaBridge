// Game events read from the screen image, for games whose own lighting can't reach
// LumaBridge (Battlefield: EA's anti-cheat keeps LumaBridge out, and its LIGHTSYNC only goes
// to G HUB). Like Screen colors it only looks at the finished image, never the game.
//
// What shooters show on screen, without knowing a game's HUD layout:
//   - taking damage: a red flash at the screen's edges (and not in the middle);
//   - low health: that red edge staying;
//   - death: the image turning grey (desaturated) for a while.
// Otherwise the screen's colors show as usual. The thresholds are a first estimate and are
// logged on every change, so they can be tuned from a real session. Pure C++, tested.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "color.h"
#include "effects.h"
#include "screen_colors.h"

namespace luma::app::games {

// What one frame shows (from the same tiny image Screen colors reads).
struct HudSignals {
    double edgeRed = 0;    // 0..1: how red the screen's border is
    double centerRed = 0;  // 0..1: how red its middle is
    double saturation = 0; // 0..1: how colorful the whole image is
    double brightness = 0; // 0..1
};

// `bgra`: w*h pixels (B, G, R, A), row-major.
inline HudSignals ReadHud(const std::vector<uint8_t>& bgra, int w, int h) {
    HudSignals s;
    double edge = 0, edgeN = 0, center = 0, centerN = 0, sat = 0, bright = 0, n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            if (i + 2 >= bgra.size()) break;
            const double b = bgra[i] / 255.0, g = bgra[i + 1] / 255.0, r = bgra[i + 2] / 255.0;
            const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
            // Red: how far red stands above the other two, counting bright pixels more.
            const double red = std::max(0.0, r - std::max(g, b)) * (0.5 + 0.5 * r);
            const bool border = x < w * 0.12 || x >= w * 0.88 || y < h * 0.15 || y >= h * 0.85;
            const bool middle = x >= w * 0.3 && x < w * 0.7 && y >= h * 0.3 && y < h * 0.7;
            if (border) {
                edge += red;
                ++edgeN;
            }
            if (middle) {
                center += red;
                ++centerN;
            }
            sat += mx > 0.04 ? (mx - mn) / mx : 0;
            bright += mx;
            ++n;
        }
    if (edgeN) s.edgeRed = edge / edgeN;
    if (centerN) s.centerRed = center / centerN;
    if (n) {
        s.saturation = sat / n;
        s.brightness = bright / n;
    }
    return s;
}

class HudLighting {
public:
    enum class State { Normal, Hit, LowHealth, Dead };

    // Thresholds (first estimates).
    static constexpr double kRedEdge = 0.10;      // border this red...
    static constexpr double kRedMargin = 0.06;    // ...and this much redder than the middle: hit
    static constexpr double kGrey = 0.07;         // saturation below this...
    static constexpr double kGreyMinBright = 0.05;  // ...on a screen that isn't just black: dead
    static constexpr uint64_t kHitMs = 450;       // a hit flashes this long
    static constexpr uint64_t kLowHealthMs = 1500; // red edges this long: low health
    static constexpr uint64_t kDeadMs = 800;      // grey this long: dead

    // Feed each new frame's signals. Returns true when the state changed.
    bool Update(const HudSignals& s, uint64_t now) {
        const State before = state_;
        const bool red = s.edgeRed > kRedEdge && s.edgeRed - s.centerRed > kRedMargin;
        const bool grey = s.saturation < kGrey && s.brightness > kGreyMinBright;
        if (red) {
            if (!redSince_) redSince_ = now;
            lastHit_ = now;
        } else {
            redSince_ = 0;
        }
        if (grey) {
            if (!greySince_) greySince_ = now;
        } else {
            greySince_ = 0;
        }
        if (greySince_ && now - greySince_ >= kDeadMs) state_ = State::Dead;
        else if (redSince_ && now - redSince_ >= kLowHealthMs) state_ = State::LowHealth;
        else if (lastHit_ && now - lastHit_ < kHitMs) state_ = State::Hit;
        else state_ = State::Normal;
        last_ = s;
        return state_ != before;
    }

    State state() const { return state_; }
    const HudSignals& last() const { return last_; }

    // The lighting: the screen's colors, a red flash on a hit (fading back), red breathing at
    // low health, dim grey-red while dead.
    fx::Params Output(const ScreenColors& screen, uint64_t now) const {
        fx::Params p;
        p.kind = fx::Kind::Gradient;
        p.color1 = screen.left;
        p.color2 = screen.right;
        p.speed = 0;
        switch (state_) {
        case State::Dead:
            p.kind = fx::Kind::Static;
            p.color1 = Rgb{40, 8, 8};
            break;
        case State::LowHealth:
            p.kind = fx::Kind::Breathing;
            p.color1 = Rgb{255, 0, 0};
            p.speed = 1.2;
            break;
        case State::Hit: {
            // Full red at the hit, back to the screen's colors over kHitMs.
            const double t = lastHit_ && now > lastHit_ ? std::min(1.0, (now - lastHit_) / double(kHitMs)) : 0;
            p.color1 = fx::detail::Lerp(Rgb{255, 0, 0}, screen.left, t);
            p.color2 = fx::detail::Lerp(Rgb{255, 0, 0}, screen.right, t);
            break;
        }
        default: break;
        }
        return p;
    }

    static const char* Name(State s) {
        switch (s) {
        case State::Hit: return "hit";
        case State::LowHealth: return "low health";
        case State::Dead: return "dead";
        default: return "normal";
        }
    }

private:
    State state_ = State::Normal;
    uint64_t redSince_ = 0, lastHit_ = 0, greySince_ = 0;
    HudSignals last_;
};

}  // namespace luma::app::games
