// The desk's DualSense model, in centimetres, with only the lightbar lit.
#pragma once

#include "scene3d.h"

namespace luma::app::controller3d {

// Surface markings use faces so they share the buttons' draw order and picking identity.
inline void Mark(s3d::Scene& sc, s3d::V3 a, s3d::V3 b, uint32_t color, int id) {
    const s3d::V3 d = s3d::Normalize(b - a);
    const s3d::V3 edge{-d.z * 0.025f, 0, d.x * 0.025f};
    const float bias = sc.bias;
    sc.bias -= 1.5f;
    sc.Quad(a + edge, b + edge, b - edge, a - edge, color, id);
    sc.bias = bias;
}

// Three rings give the shell sloping shoulders and tapered edges instead of square boxes.
inline void Shell(s3d::Scene& sc, const std::array<s3d::V3, 8>& outline, float bottom, float top,
                  uint32_t color, int id, float slope = 0) {
    std::array<s3d::V3, 8> low{}, rim{}, crown{}, underside{};
    for (size_t i = 0; i < outline.size(); ++i) {
        const auto p = outline[i];
        low[i] = {p.x * 0.88f, bottom, p.z * 0.88f};
        rim[i] = {p.x, top - std::min(0.55f, (top - bottom) * 0.45f) - slope * p.z, p.z};
        crown[i] = {p.x * 0.89f, top - slope * p.z, p.z * 0.89f};
        underside[outline.size() - 1 - i] = low[i];
    }
    sc.Poly(underside.data(), 8, color, id);
    sc.Poly(crown.data(), 8, color, id);
    for (size_t i = 0; i < outline.size(); ++i) {
        const size_t j = (i + 1) % outline.size();
        sc.Quad(low[i], low[j], rim[j], rim[i], color, id);
        sc.Quad(rim[i], rim[j], crown[j], crown[i], color, id);
    }
}

inline void Build(s3d::Scene& sc, int id, uint32_t lightbar, bool lit) {
    using namespace s3d;
    const Transform at = sc.xf;
    const float oldBias = sc.bias;
    const uint32_t white = Rgba(211, 215, 222), black = Rgba(24, 27, 34), rubber = Rgba(42, 46, 55);
    const std::array<V3, 8> body{{{-5.5f, 0, -4.4f}, {-6.8f, 0, -3.3f}, {-6.8f, 0, 0.7f}, {-5.5f, 0, 1.9f},
                                  {5.5f, 0, 1.9f}, {6.8f, 0, 0.7f}, {6.8f, 0, -3.3f}, {5.5f, 0, -4.4f}}};
    const std::array<V3, 8> grip{{{-1.05f, 0, -1.5f}, {-1.9f, 0, -0.6f}, {-1.7f, 0, 3.9f}, {-0.9f, 0, 4.9f},
                                  {0.9f, 0, 4.9f}, {1.7f, 0, 3.9f}, {1.9f, 0, -0.6f}, {1.05f, 0, -1.5f}}};
    // Bias keeps the central body behind its overlapping grip shells in the painter renderer.
    sc.bias = 4;
    Shell(sc, body, 0.35f, 3.2f, white, id);
    for (int side = 0; side < 2; ++side) {
        const float s = side ? 1.f : -1.f;
        sc.xf = Transform::YawAt(s * 0.27f, {s * 4.75f, 0, 0.8f}).Then(at);
        sc.bias = 2;
        Shell(sc, grip, 0.3f, 3.35f, white, id, 0.16f);
    }
    sc.xf = at;
    sc.bias = 0;
    const std::array<V3, 8> middle{{{-2.65f, 0, -4.2f}, {-3.3f, 0, -3.5f}, {-3.4f, 0, 1.7f}, {-2.5f, 0, 2.8f},
                                    {2.5f, 0, 2.8f}, {3.4f, 0, 1.7f}, {3.3f, 0, -3.5f}, {2.65f, 0, -4.2f}}};
    Shell(sc, middle, 2.5f, 3.4f, black, id);
    const std::array<V3, 8> pad{{{-2.3f, 0, -1.35f}, {2.3f, 0, -1.35f}, {2.85f, 0, -1.8f}, {2.85f, 0, -3.75f},
                                 {2.45f, 0, -4.05f}, {-2.45f, 0, -4.05f}, {-2.85f, 0, -3.75f}, {-2.85f, 0, -1.8f}}};
    sc.bias = -2;
    Shell(sc, pad, 3.35f, 3.75f, Rgba(48, 52, 61), id);
    // Light strips are on the upper face, pointing up, along both touchpad edges.
    sc.bias = -3.5f;
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 2.9f : -3.15f;
        sc.Quad({x, 3.78f, -1.6f}, {x + 0.25f, 3.78f, -1.6f}, {x + 0.25f, 3.78f, -3.8f}, {x, 3.78f, -3.8f},
                lightbar, id, lit ? kEmissive : 0u);
        if (lit) sc.AddGlow({x + 0.125f, 3.85f, -2.7f}, 2.f, WithAlpha(lightbar, 80));
    }
    sc.bias = -2;
    for (int i = 0; i < 5; ++i) {
        const float x = -0.8f + 0.4f * static_cast<float>(i);
        sc.Box({x - 0.08f, 3.41f, -1.1f}, {x + 0.08f, 3.45f, -0.9f}, rubber, id);
    }
    // Separate shoulder buttons and triggers on the back edge.
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 3.6f : -6.0f;
        sc.Box({x, 1.1f, -4.8f}, {x + 2.4f, 2.5f, -4.35f}, black, id);
        sc.Box({x, 2.65f, -4.55f}, {x + 2.4f, 3.15f, -4.1f}, rubber, id);
    }
    auto button = [&](float x, float y, float z, float r, float height, uint32_t color) {
        sc.xf = Transform::Facing({0, 1, 0}, {x, y, z}).Then(at);
        sc.Disc(r, height, 8, color, id);
        for (int i = 0; i < 8; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / 8, b = 6.2831853f * static_cast<float>(i + 1) / 8;
            sc.Quad({r * std::cos(a), r * std::sin(a), 0}, {r * std::cos(b), r * std::sin(b), 0},
                    {r * std::cos(b), r * std::sin(b), height}, {r * std::cos(a), r * std::sin(a), height}, color, id);
        }
        sc.xf = at;
    };
    // The D-pad has four separate directions and a recessed center.
    button(-4.65f, 3.2f, -2.4f, 0.55f, 0.08f, black);
    for (int i = 0; i < 4; ++i) {
        sc.xf = Transform::YawAt(1.5707963f * static_cast<float>(i), {-4.65f, 0, -2.4f}).Then(at);
        sc.Box({-0.3f, 3.25f, -1.05f}, {0.3f, 3.65f, -0.2f}, rubber, id);
    }
    sc.xf = at;
    const uint32_t mark = Rgba(172, 179, 192);
    for (int i = 0; i < 4; ++i) {
        const float x = 4.65f + (i == 0 ? -0.8f : i == 1 ? 0.8f : 0);
        const float z = -2.4f + (i == 2 ? -0.8f : i == 3 ? 0.8f : 0);
        button(x, 3.25f, z, 0.42f, 0.35f, Rgba(101, 108, 121));
        auto line = [&](float ax, float az, float bx, float bz) {
            Mark(sc, {x + ax, 3.62f, z + az}, {x + bx, 3.62f, z + bz}, mark, id);
        };
        if (i == 0) {  // square
            line(-0.18f, -0.18f, 0.18f, -0.18f); line(0.18f, -0.18f, 0.18f, 0.18f);
            line(0.18f, 0.18f, -0.18f, 0.18f); line(-0.18f, 0.18f, -0.18f, -0.18f);
        } else if (i == 1) {  // circle
            for (int j = 0; j < 8; ++j) {
                const float a = 6.2831853f * static_cast<float>(j) / 8, b = 6.2831853f * static_cast<float>(j + 1) / 8;
                line(0.2f * std::cos(a), 0.2f * std::sin(a), 0.2f * std::cos(b), 0.2f * std::sin(b));
            }
        } else if (i == 2) {
            line(0, -0.22f, 0.2f, 0.16f); line(0.2f, 0.16f, -0.2f, 0.16f); line(-0.2f, 0.16f, 0, -0.22f);
        } else {
            line(-0.17f, -0.17f, 0.17f, 0.17f); line(-0.17f, 0.17f, 0.17f, -0.17f);
        }
    }
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 2.05f : -2.05f;
        button(x, 3.35f, 1.15f, 1.15f, 0.15f, black);
        button(x, 3.5f, 1.15f, 0.4f, 0.5f, black);
        button(x, 4.f, 1.15f, 0.94f, 0.2f, rubber);
        button(x, 4.2f, 1.15f, 0.68f, 0.02f, Rgba(55, 60, 69));
    }
    button(0, 3.45f, 1.3f, 0.36f, 0.12f, rubber);  // PS button
    sc.Box({-0.25f, 3.4f, 2.1f}, {0.25f, 3.53f, 2.35f}, rubber, id);  // microphone mute
    Mark(sc, {0, 3.58f, 1.13f}, {0, 3.58f, 1.47f}, mark, id);
    Mark(sc, {0, 3.58f, 1.13f}, {0.17f, 3.58f, 1.22f}, mark, id);
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 3.35f : -3.65f;
        sc.Box({x, 3.3f, -3.7f}, {x + 0.3f, 3.65f, -3.1f}, rubber, id);  // Create and Options
    }
    for (int i = 0; i < 6; ++i) {
        const float x = -0.65f + static_cast<float>(i % 3) * 0.65f, z = -0.5f + static_cast<float>(i / 3) * 0.3f;
        button(x, 3.43f, z, 0.06f, 0.03f, Rgba(13, 15, 19));  // speaker holes
    }
    sc.xf = at;
    sc.bias = oldBias;
}

}  // namespace luma::app::controller3d
