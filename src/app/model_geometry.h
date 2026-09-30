// Small, closed shapes shared by the desk and PC models. Faces point outwards.
#pragma once

#include "scene3d.h"

namespace luma::app::model3d {
using s3d::V3;

inline void BevelBox(s3d::Scene& sc, V3 lo, V3 hi, float bevel, uint32_t color, int id, uint32_t flags = 0) {
    const float b = std::min({bevel, (hi.x - lo.x) * 0.2f, (hi.y - lo.y) * 0.25f, (hi.z - lo.z) * 0.2f});
    if (b <= 0) return;
    const float x = (lo.x + hi.x) / 2, z = (lo.z + hi.z) / 2;
    const std::array<V3, 8> ring{{{lo.x + b, 0, lo.z}, {lo.x, 0, lo.z + b}, {lo.x, 0, hi.z - b}, {lo.x + b, 0, hi.z},
                                  {hi.x - b, 0, hi.z}, {hi.x, 0, hi.z - b}, {hi.x, 0, lo.z + b}, {hi.x - b, 0, lo.z}}};
    std::array<V3, 8> rings[4], underside;
    const float heights[] = {lo.y, lo.y + b, hi.y - b, hi.y};
    for (int r = 0; r < 4; ++r)
        for (size_t i = 0; i < ring.size(); ++i) {
            V3 p = ring[i];
            if (r == 0 || r == 3) {
                p.x += (p.x < x ? b : -b) * 0.65f;
                p.z += (p.z < z ? b : -b) * 0.65f;
            }
            p.y = heights[r]; rings[r][i] = p;
        }
    for (size_t i = 0; i < ring.size(); ++i) underside[7 - i] = rings[0][i];
    sc.Poly(underside.data(), 8, color, id, flags);
    sc.Poly(rings[3].data(), 8, color, id, flags);
    for (int r = 0; r < 3; ++r)
        for (size_t i = 0; i < ring.size(); ++i) {
            const size_t j = (i + 1) % ring.size();
            sc.Quad(rings[r][i], rings[r][j], rings[r + 1][j], rings[r + 1][i], color, id, flags);
        }
}

// A cylinder in the local xy plane, along z, with closed caps.
inline void Cylinder(s3d::Scene& sc, float radius, float lo, float hi, uint32_t color, int id, int sides = 16) {
    sides = std::clamp(sides, 6, 16);
    std::array<V3, 16> front{}, back{};
    for (int i = 0; i < sides; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(sides);
        front[static_cast<size_t>(i)] = {std::cos(a) * radius, std::sin(a) * radius, hi};
        back[static_cast<size_t>(sides - 1 - i)] = {std::cos(a) * radius, std::sin(a) * radius, lo};
    }
    sc.Poly(front.data(), sides, color, id);
    sc.Poly(back.data(), sides, color, id);
    for (int i = 0; i < sides; ++i) {
        const V3 a = front[static_cast<size_t>(i)], b = front[static_cast<size_t>((i + 1) % sides)];
        sc.Quad({a.x, a.y, lo}, {b.x, b.y, lo}, b, a, color, id);
    }
}

inline void Tube(s3d::Scene& sc, V3 a, V3 b, float radius, uint32_t color, int id) {
    const auto at = sc.xf;
    sc.xf = s3d::Transform::Facing(b - a, a).Then(at);
    Cylinder(sc, radius, 0, s3d::Length(b - a), color, id, 10);
    sc.xf = at;
}

// Five rows of three pixels make small legends readable without floating text labels.
inline const char* Glyph(char c) {
    switch (c) {
    case 'A': return "010101111101101"; case 'B': return "110101110101110";
    case 'C': return "011100100100011"; case 'D': return "110101101101110";
    case 'E': return "111100110100111"; case 'F': return "111100110100100";
    case 'G': return "011100101101011"; case 'H': return "101101111101101";
    case 'I': return "111010010010111"; case 'J': return "001001001101010";
    case 'K': return "101101110101101"; case 'L': return "100100100100111";
    case 'M': return "101111111101101"; case 'N': return "101111111111101";
    case 'O': return "010101101101010"; case 'P': return "110101110100100";
    case 'Q': return "010101101111011"; case 'R': return "110101110101101";
    case 'S': return "011100010001110"; case 'T': return "111010010010010";
    case 'U': return "101101101101111"; case 'V': return "101101101101010";
    case 'W': return "101101111111101"; case 'X': return "101101010101101";
    case 'Y': return "101101010010010"; case 'Z': return "111001010100111";
    case '0': return "111101101101111"; case '1': return "010110010010111";
    case '2': return "110001010100111"; case '3': return "110001010001110";
    case '4': return "101101111001001"; case '5': return "111100110001110";
    case '6': return "011100111101111"; case '7': return "111001010010010";
    case '8': return "111101111101111"; case '9': return "111101111001110";
    case '-': return "000000111000000"; case '+': return "000010111010000";
    case '<': return "001010100010001"; case '>': return "100010001010100";
    case '^': return "010101000000000"; case '.': return "000000000000010";
    default: return "000000000000000";
    }
}

inline void Legend(s3d::Scene& sc, const char* text, float cx, float y, float cz, float pixel, uint32_t color, int id, bool lit) {
    size_t count = 0;
    while (text[count]) ++count;
    if (!count) return;
    const float start = cx - static_cast<float>(count * 4 - 1) * pixel / 2;
    for (size_t k = 0; k < count; ++k) {
        const char* glyph = Glyph(text[k]);
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 3; ++col) {
                if (glyph[row * 3 + col] != '1') continue;
                const float x = start + (static_cast<float>(k * 4) + static_cast<float>(col)) * pixel;
                const float z = cz + (static_cast<float>(row) - 2.5f) * pixel;
                sc.Quad({x, y, z + pixel * 0.86f}, {x + pixel * 0.86f, y, z + pixel * 0.86f},
                        {x + pixel * 0.86f, y, z}, {x, y, z}, color, id, lit ? s3d::kEmissive : 0u);
            }
    }
}
}  // namespace luma::app::model3d
