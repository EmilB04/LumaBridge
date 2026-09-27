// Screen colors: turns a coarse grid of screen samples into the colors the fans show (left
// half and right half of the screen, blended around each fan). Near-black and near-white
// pixels count less, and the result is made more vivid, because an honest average of a game
// scene is mostly grey-brown. Pure C++, tested.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "color.h"

namespace luma::app::games {

struct ScreenColors {
    Rgb left, right;
};

// `bgra`: w*h pixels, 4 bytes each (B, G, R, A), row-major.
inline ScreenColors SummarizeScreen(const std::vector<uint8_t>& bgra, int w, int h) {
    double sum[2][3] = {}, weight[2] = {};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            if (i + 2 >= bgra.size()) break;
            const double b = bgra[i], g = bgra[i + 1], r = bgra[i + 2];
            const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
            const double sat = mx > 0 ? (mx - mn) / mx : 0;
            // Colorful, reasonably bright pixels dominate.
            const double wgt = 0.05 + sat * sat * 3 + (mx > 40 ? 0.3 : 0);
            const int side = x < w / 2 ? 0 : 1;
            sum[side][0] += r * wgt;
            sum[side][1] += g * wgt;
            sum[side][2] += b * wgt;
            weight[side] += wgt;
        }
    auto finish = [&](int side) {
        if (weight[side] <= 0) return Rgb{};
        double r = sum[side][0] / weight[side], g = sum[side][1] / weight[side], b = sum[side][2] / weight[side];
        // Push saturation up and scale so the brightest channel is at least 60 % (dark
        // scenes still show their hue; black stays black).
        const double avg = (r + g + b) / 3;
        r = avg + (r - avg) * 1.6;
        g = avg + (g - avg) * 1.6;
        b = avg + (b - avg) * 1.6;
        const double mx = std::max({r, g, b, 1.0});
        const double target = mx < 12 ? mx : std::max(mx, 153.0);
        const double k = target / mx;
        return Rgb{ClampByte(r * k), ClampByte(g * k), ClampByte(b * k)};
    };
    return ScreenColors{finish(0), finish(1)};
}

}  // namespace luma::app::games
