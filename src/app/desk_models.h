#pragma once

#include "model_geometry.h"
#include "azoth_layout.h"

namespace luma::app::desk3d {
using namespace s3d;

inline const char* KeyLegend(int led) {
    switch (led) {
    case 0: return "ESC"; case 8: return "F1"; case 16: return "F2"; case 24: return "F3"; case 32: return "F4";
    case 40: return "F5"; case 48: return "F6"; case 56: return "F7"; case 64: return "F8";
    case 72: return "F9"; case 80: return "F10"; case 88: return "F11"; case 96: return "F12";
    case 1: return "<"; case 9: return "1"; case 17: return "2"; case 25: return "3"; case 33: return "4";
    case 41: return "5"; case 49: return "6"; case 57: return "7"; case 65: return "8"; case 73: return "9";
    case 81: return "0"; case 89: return "+"; case 97: return "-"; case 105: return "<";
    case 2: return ">"; case 10: return "Q"; case 18: return "W"; case 26: return "E"; case 34: return "R";
    case 42: return "T"; case 50: return "Y"; case 58: return "U"; case 66: return "I"; case 74: return "O";
    case 82: return "P"; case 90: return "A"; case 98: return "^";
    case 3: return "CAP"; case 11: return "A"; case 19: return "S"; case 27: return "D"; case 35: return "F";
    case 43: return "G"; case 51: return "H"; case 59: return "J"; case 67: return "K"; case 75: return "L";
    case 83: return "O"; case 91: return "A"; case 99: return "^"; case 107: return "ENT";
    case 4: case 100: return "^"; case 12: return "<"; case 20: return "Z"; case 28: return "X";
    case 36: return "C"; case 44: return "V"; case 52: return "B"; case 60: return "N"; case 68: return "M";
    case 76: return "."; case 84: return "."; case 92: return "-"; case 116: return "^";
    case 5: case 101: return "CTL"; case 13: return "WIN"; case 21: case 85: return "ALT"; case 93: return "FN";
    case 109: return "<"; case 117: return "V"; case 125: return ">";
    case 121: return "DEL"; case 122: return "PGU"; case 123: return "PGD"; case 124: return "END";
    default: return "";
    }
}

inline void Keyboard(Scene& sc, int id, const std::vector<Rgb>& colors, bool lit, bool azothModel) {
    const Transform at = sc.xf;
    const float bias = sc.bias;
    sc.bias = 0;
    model3d::BevelBox(sc, {-16.8f, 0.2f, -7.0f}, {16.8f, 1.2f, 6.7f}, 0.3f, Rgba(30, 34, 42), id);
    model3d::BevelBox(sc, {-16.55f, 1.2f, -6.8f}, {16.55f, 1.45f, 6.5f}, 0.15f, Rgba(85, 94, 109), id);
    sc.Box({-16.15f, 1.45f, -6.5f}, {16.15f, 1.55f, 6.2f}, Rgba(20, 23, 29), id);
    for (float x : {-12.f, 12.f}) {
        sc.Box({x - 1.4f, 0, -5.8f}, {x + 1.4f, 0.2f, -4.4f}, Rgba(12, 15, 20), id);
        sc.Box({x - 1.4f, 0, 4.4f}, {x + 1.4f, 0.2f, 5.8f}, Rgba(12, 15, 20), id);
    }
    const auto& keys = azoth::IsoKeys();
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        const float x0 = -16 + key.x * 2 + 0.12f, x1 = x0 + key.w * 2 - 0.24f;
        const float z0 = -6.3f + key.y * 2.05f + 0.12f, z1 = z0 + key.h * 2 - 0.24f;
        const float y = 1.72f - (z0 + z1) / 2 * 0.022f;
        const Rgb c = i < colors.size() ? colors[i] : Rgb{95, 107, 127};
        sc.Box({x0, 1.56f, z0}, {x1, y + 0.08f, z1}, lit ? Rgba(c.r / 4, c.g / 4, c.b / 4) : Rgba(37, 41, 49), id,
               lit ? kEmissive : 0u);
        model3d::BevelBox(sc, {x0, y + 0.08f, z0}, {x1, y + 0.78f, z1}, 0.2f, Rgba(46, 51, 60), id);
        const char* text = KeyLegend(key.led);
        const uint32_t legend = lit ? Rgba(c.r, c.g, c.b) : Rgba(174, 183, 197);
        model3d::Legend(sc, text, (x0 + x1) / 2, y + 0.8f, (z0 + z1) / 2, 0.115f, legend, id, lit);
        if (key.led == 53) model3d::Legend(sc, "-", (x0 + x1) / 2, y + 0.8f, (z0 + z1) / 2, 0.16f, legend, id, lit);
    }
    // USB-C socket and the Azoth's OLED and control knob, separate from its keys.
    sc.Box({-0.55f, 0.7f, -7.02f}, {0.55f, 1.08f, -6.96f}, Rgba(8, 10, 14), id);
    if (azothModel) {
        model3d::BevelBox(sc, {11.65f, 1.6f, -6.2f}, {14.35f, 2.65f, -4.4f}, 0.15f, Rgba(18, 21, 26), id);
        sc.Quad({11.85f, 2.68f, -4.6f}, {14.15f, 2.68f, -4.6f}, {14.15f, 2.68f, -6}, {11.85f, 2.68f, -6}, Rgba(11, 18, 25), id);
        model3d::Legend(sc, "ROG", 13, 2.7f, -5.3f, 0.12f, Rgba(132, 195, 203), id, true);
        sc.xf = Transform::Facing({0, 1, 0}, {15.25f, 1.6f, -5.35f}).Then(at);
        model3d::Cylinder(sc, 0.55f, 0, 1.25f, Rgba(83, 91, 104), id);
        sc.Disc(0.42f, 1.27f, 16, Rgba(27, 31, 39), id);
        sc.xf = at;
    }
    sc.bias = bias;
}

inline V3 MouseSurface(float z, float across) {
    constexpr float zs[] = {-6.2f, -5.3f, -3.2f, 0, 3, 5.4f, 6.2f};
    constexpr float widths[] = {1.5f, 2.5f, 3, 3.2f, 2.95f, 1.9f, 0.35f};
    constexpr float heights[] = {1.6f, 2.35f, 3.15f, 4.05f, 3.75f, 2.4f, 1.2f};
    z = std::clamp(z, zs[0], zs[6]);
    int i = 0;
    while (i < 5 && z > zs[i + 1]) ++i;
    const float t = (z - zs[i]) / (zs[i + 1] - zs[i]);
    const float width = widths[i] + (widths[i + 1] - widths[i]) * t;
    const float height = heights[i] + (heights[i + 1] - heights[i]) * t;
    return {width * across, 0.85f + (height - 0.85f) * std::cos(across * 1.5707963f), z};
}

// Clip a surface detail to each shell triangle. A single large quad cuts through a curved
// shell, making light strips and button panels vanish in patches when the view changes.
inline void MousePatch(Scene& sc, float z0, float z1, float a0, float a1, float lift,
                       uint32_t color, int id, uint32_t flags = 0) {
    constexpr float zs[] = {-6.2f, -5.3f, -3.2f, 0, 3, 5.4f, 6.2f};
    struct Vertex { V3 p; float across; };
    for (int row = 0; row < 6; ++row)
        for (int col = 0; col < 8; ++col) {
            const float a = -1 + static_cast<float>(col) / 4, b = a + 0.25f;
            if (zs[row + 1] <= z0 || zs[row] >= z1 || b <= a0 || a >= a1) continue;
            const Vertex corners[] = {{MouseSurface(zs[row], a), a}, {MouseSurface(zs[row + 1], a), a},
                                      {MouseSurface(zs[row + 1], b), b}, {MouseSurface(zs[row], b), b}};
            for (int tri = 0; tri < 2; ++tri) {
                std::array<Vertex, 12> polygon{}, clipped{};
                polygon[0] = corners[0]; polygon[1] = corners[tri + 1]; polygon[2] = corners[tri + 2];
                int n = 3;
                const float bounds[] = {z0, z1, a0, a1};
                for (int plane = 0; plane < 4 && n >= 3; ++plane) {
                    auto distance = [&](const Vertex& v) {
                        const float p = plane < 2 ? v.p.z : v.across;
                        return plane % 2 ? bounds[plane] - p : p - bounds[plane];
                    };
                    int count = 0;
                    for (int i = 0; i < n; ++i) {
                        const Vertex& prev = polygon[static_cast<size_t>((i + n - 1) % n)];
                        const Vertex& next = polygon[static_cast<size_t>(i)];
                        const float d0 = distance(prev), d1 = distance(next);
                        if ((d0 >= 0) != (d1 >= 0)) {
                            const float t = d0 / (d0 - d1);
                            clipped[static_cast<size_t>(count++)] = {Lerp(prev.p, next.p, t), prev.across + (next.across - prev.across) * t};
                        }
                        if (d1 >= 0) clipped[static_cast<size_t>(count++)] = next;
                    }
                    n = 0;
                    for (int i = 0; i < count; ++i)
                        if (!n || Length(clipped[static_cast<size_t>(i)].p - polygon[static_cast<size_t>(n - 1)].p) > 0.00001f)
                            polygon[static_cast<size_t>(n++)] = clipped[static_cast<size_t>(i)];
                    if (n > 1 && Length(polygon[0].p - polygon[static_cast<size_t>(n - 1)].p) < 0.00001f) --n;
                }
                if (n < 3) continue;
                std::array<V3, 12> points{};
                for (int i = 0; i < n; ++i) {
                    points[static_cast<size_t>(i)] = polygon[static_cast<size_t>(i)].p;
                    points[static_cast<size_t>(i)].y += lift;
                }
                // Clipping exactly along a triangle edge can leave a zero-area polygon.
                if (Length(Cross(points[1] - points[0], points[2] - points[0])) > 0.000001f)
                    sc.Poly(points.data(), n, color, id, flags);
            }
        }
}

inline void Mouse(Scene& sc, int id, const std::vector<Rgb>& colors, bool lit) {
    const Transform at = sc.xf;
    const float bias = sc.bias;
    sc.bias = 0;
    constexpr float zs[] = {-6.2f, -5.3f, -3.2f, 0, 3, 5.4f, 6.2f};
    std::array<V3, 14> bottom;
    for (int i = 0; i < 7; ++i) {
        V3 p = MouseSurface(zs[i], 1); p.y = 0.15f;
        bottom[static_cast<size_t>(i)] = p;
        p.x = -p.x;
        bottom[static_cast<size_t>(13 - i)] = p;
    }
    // The underside and side walls follow the same silhouette as the curved top.
    sc.Poly(bottom.data(), 14, Rgba(18, 21, 27), id);
    for (int row = 0; row < 6; ++row)
        for (int side = 0; side < 2; ++side) {
            const float s = side ? 1.f : -1.f;
            V3 a = MouseSurface(zs[row], s), b = MouseSurface(zs[row + 1], s);
            V3 lowA = a, lowB = b; lowA.y = lowB.y = 0.15f;
            if (side) sc.Quad(a, b, lowB, lowA, Rgba(29, 34, 42), id);
            else sc.Quad(a, lowA, lowB, b, Rgba(29, 34, 42), id);
        }
    for (int end : {0, 6}) {
        std::array<V3, 11> cap{};
        cap[0] = MouseSurface(zs[end], -1); cap[0].y = 0.15f;
        for (int i = 0; i <= 8; ++i) cap[static_cast<size_t>(i + 1)] = MouseSurface(zs[end], -1 + static_cast<float>(i) / 4);
        cap[10] = MouseSurface(zs[end], 1); cap[10].y = 0.15f;
        if (end == 6) std::reverse(cap.begin(), cap.end());
        sc.Poly(cap.data(), static_cast<int>(cap.size()), Rgba(29, 34, 42), id);
    }
    // More strips around the arch keep the hump smooth from both sides.
    for (int row = 0; row < 6; ++row)
        for (int col = 0; col < 8; ++col) {
            const float a = -1 + static_cast<float>(col) / 4, b = a + 0.25f;
            sc.Quad(MouseSurface(zs[row], a), MouseSurface(zs[row + 1], a), MouseSurface(zs[row + 1], b), MouseSurface(zs[row], b),
                    Rgba(41, 46, 56), id);
        }
    for (int side = 0; side < 2; ++side) {
        const float edge = side ? 0.92f : -0.92f, inner = side ? 0.08f : -0.08f;
        MousePatch(sc, zs[1], zs[3] - 0.1f, std::min(edge, inner), std::max(edge, inner), 0.12f,
                   Rgba(62, 68, 81), id);
        for (int i = 0; i < 4; ++i) {
            const int zone = side ? 7 - i : i;
            const Rgb color = static_cast<size_t>(zone) < colors.size() ? colors[static_cast<size_t>(zone)] : Rgb{72, 80, 95};
            const float a = side ? 0.76f : -0.97f, b = side ? 0.97f : -0.76f;
            MousePatch(sc, -0.3f + 1.4f * i, 1.1f + 1.4f * i, a, b, 0.04f,
                       Rgba(color.r, color.g, color.b), id, lit ? kEmissive : 0u);
        }
    }
    // Thumb rest, two thumb buttons, DPI buttons, and a grooved wheel on its axle.
    model3d::BevelBox(sc, {-4.1f, 0.2f, -0.5f}, {-2.9f, 0.7f, 4.1f}, 0.2f, Rgba(28, 33, 42), id);
    for (int i = 0; i < 2; ++i)
        model3d::BevelBox(sc, {-3.18f, 1.4f, -1.8f + 1.4f * i}, {-2.85f, 1.9f, -0.7f + 1.4f * i}, 0.1f, Rgba(96, 106, 121), id);
    sc.xf = Transform::Facing({1, 0, 0}, {-0.3f, 2.95f, -4.25f}).Then(at);
    model3d::Cylinder(sc, 0.7f, 0, 0.6f, Rgba(92, 103, 119), id);
    for (int i = 0; i < 16; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 16;
        sc.AddLine({0.705f * std::cos(a), 0.705f * std::sin(a), 0.05f},
                   {0.705f * std::cos(a), 0.705f * std::sin(a), 0.55f}, Rgba(21, 25, 32), 1);
    }
    sc.xf = at;
    for (int i = 0; i < 2; ++i) {
        const float z = -1.8f + 0.7f * i;
        model3d::BevelBox(sc, {-0.36f, 3.7f, z}, {0.36f, 3.96f, z + 0.45f}, 0.1f, Rgba(83, 93, 108), id);
    }
    sc.bias = bias;
}
}  // namespace luma::app::desk3d
