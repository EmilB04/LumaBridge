// The desk's DualSense model, in centimetres, with only the lightbar lit. The controller page
// draws the same model live: held buttons light up and sink, sticks and triggers move, and
// fingers show on the touchpad.
#pragma once

#include "pad_protocol.h"
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

// Between two colors (0 = a, 1 = b), alpha included.
inline uint32_t Mix(uint32_t a, uint32_t b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xFF), cb = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<uint32_t>(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

// `live`: the controller's state to show (the controller page), with held parts in `press`;
// null for the desk, where it sits still.
inline void Build(s3d::Scene& sc, int id, uint32_t lightbar, bool lit, const pad::State* live = nullptr,
                  uint32_t press = 0) {
    using namespace s3d;
    auto held = [&](int b) { return live && live->Down(b); };
    auto tint = [&](uint32_t base, int b) { return held(b) ? press : base; };
    auto glowing = [&](int b) { return held(b) ? kEmissive : 0u; };
    auto sink = [&](int b, float depth) { return held(b) ? depth : 0.f; };
    auto axis = [](uint8_t v) { return std::clamp((static_cast<float>(v) - 128.f) / 127.f, -1.f, 1.f); };
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
    Shell(sc, pad, 3.35f, 3.75f - sink(pad::kTouchpad, 0.08f), tint(Rgba(48, 52, 61), pad::kTouchpad), id,
          0);
    // Fingers on the touchpad (its surface runs 0..1919 across and 0..1079 down).
    if (live)
        for (const auto& touch : live->touch) {
            if (!touch.down) continue;
            const float x = -2.6f + 5.2f * std::clamp(static_cast<float>(touch.x) / 1919.f, 0.f, 1.f);
            const float z = -3.85f + 2.3f * std::clamp(static_cast<float>(touch.y) / 1079.f, 0.f, 1.f);
            sc.bias = -3;
            sc.xf = Transform::Facing({0, 1, 0}, {x, 3.77f, z}).Then(at);
            sc.Disc(0.32f, 0.02f, 12, press, id, kEmissive);
            sc.xf = at;
            sc.AddGlow({x, 3.85f, z}, 0.9f, WithAlpha(press, 110));
        }
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
    // A trigger travels down and back as it's squeezed, taking on `press` along the way.
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 3.6f : -6.0f;
        const float travel = live ? static_cast<float>(side ? live->r2 : live->l2) / 255.f : 0.f;
        const float dy = -0.5f * travel, dz = 0.25f * travel;
        sc.Box({x, 1.1f + dy, -4.8f + dz}, {x + 2.4f, 2.5f + dy, -4.35f + dz}, travel > 0.02f ? Mix(black, press, 0.25f + 0.75f * travel) : black,
               id, travel > 0.5f ? kEmissive : 0u);
        const int bumper = side ? pad::kR1 : pad::kL1;
        const float down = sink(bumper, 0.15f);
        sc.Box({x, 2.65f - down, -4.55f}, {x + 2.4f, 3.15f - down, -4.1f}, tint(rubber, bumper), id, glowing(bumper));
    }
    auto button = [&](float x, float y, float z, float r, float height, uint32_t color, uint32_t flags = 0) {
        sc.xf = Transform::Facing({0, 1, 0}, {x, y, z}).Then(at);
        sc.Disc(r, height, 8, color, id, flags);
        for (int i = 0; i < 8; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / 8, b = 6.2831853f * static_cast<float>(i + 1) / 8;
            sc.Quad({r * std::cos(a), r * std::sin(a), 0}, {r * std::cos(b), r * std::sin(b), 0},
                    {r * std::cos(b), r * std::sin(b), height}, {r * std::cos(a), r * std::sin(a), height}, color, id, flags);
        }
        sc.xf = at;
    };
    // The D-pad has four separate directions and a recessed center.
    button(-4.65f, 3.2f, -2.4f, 0.55f, 0.08f, black);
    static const int arms[4] = {pad::kUp, pad::kLeft, pad::kDown, pad::kRight};  // turning a quarter each
    for (int i = 0; i < 4; ++i) {
        sc.xf = Transform::YawAt(1.5707963f * static_cast<float>(i), {-4.65f, 0, -2.4f}).Then(at);
        const float down = sink(arms[i], 0.15f);
        sc.Box({-0.3f, 3.25f - down, -1.05f}, {0.3f, 3.65f - down, -0.2f}, tint(rubber, arms[i]), id, glowing(arms[i]));
    }
    sc.xf = at;
    const uint32_t mark = Rgba(172, 179, 192);
    static const int faces[4] = {pad::kSquare, pad::kCircle, pad::kTriangle, pad::kCross};
    for (int i = 0; i < 4; ++i) {
        const float x = 4.65f + (i == 0 ? -0.8f : i == 1 ? 0.8f : 0);
        const float z = -2.4f + (i == 2 ? -0.8f : i == 3 ? 0.8f : 0);
        const float down = sink(faces[i], 0.18f);
        button(x, 3.25f - down, z, 0.42f, 0.35f, tint(Rgba(101, 108, 121), faces[i]), glowing(faces[i]));
        const uint32_t ink = held(faces[i]) ? Rgba(255, 255, 255) : mark;
        auto line = [&](float ax, float az, float bx, float bz) {
            Mark(sc, {x + ax, 3.62f - down, z + az}, {x + bx, 3.62f - down, z + bz}, ink, id);
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
    // Sticks: the cap leans the way the stick is pushed (up on the stick is away from the player).
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 2.05f : -2.05f;
        const int click = side ? pad::kR3 : pad::kL3;
        const float dx = live ? 0.4f * axis(side ? live->rx : live->lx) : 0.f;
        const float dz = live ? 0.4f * axis(side ? live->ry : live->ly) : 0.f;
        const float down = sink(click, 0.12f);
        button(x, 3.35f, 1.15f, 1.15f, 0.15f, black);
        button(x + dx * 0.5f, 3.5f - down, 1.15f + dz * 0.5f, 0.4f, 0.5f, black);
        button(x + dx, 4.f - down, 1.15f + dz, 0.94f, 0.2f, tint(rubber, click), glowing(click));
        button(x + dx, 4.2f - down, 1.15f + dz, 0.68f, 0.02f, held(click) ? press : Rgba(55, 60, 69), glowing(click));
    }
    button(0, 3.45f - sink(pad::kPs, 0.06f), 1.3f, 0.36f, 0.12f, tint(rubber, pad::kPs), glowing(pad::kPs));  // PS button
    sc.Box({-0.25f, 3.4f, 2.1f}, {0.25f, 3.53f - sink(pad::kMute, 0.06f), 2.35f}, tint(rubber, pad::kMute), id,
           glowing(pad::kMute));  // microphone mute
    Mark(sc, {0, 3.58f, 1.13f}, {0, 3.58f, 1.47f}, mark, id);
    Mark(sc, {0, 3.58f, 1.13f}, {0.17f, 3.58f, 1.22f}, mark, id);
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 3.35f : -3.65f;
        const int b = side ? pad::kOptions : pad::kCreate;
        sc.Box({x, 3.3f, -3.7f}, {x + 0.3f, 3.65f - sink(b, 0.12f), -3.1f}, tint(rubber, b), id, glowing(b));  // Create and Options
    }
    for (int i = 0; i < 6; ++i) {
        const float x = -0.65f + static_cast<float>(i % 3) * 0.65f, z = -0.5f + static_cast<float>(i / 3) * 0.3f;
        button(x, 3.43f, z, 0.06f, 0.03f, Rgba(13, 15, 19));  // speaker holes
    }
    sc.xf = at;
    sc.bias = oldBias;
}

}  // namespace luma::app::controller3d
