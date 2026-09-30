// Chassis and component detail in the case's local frame. Fan slots and detected hardware
// remain in the UI's scene builder; these shapes add the parts around them.
#pragma once

#include "model_geometry.h"
#include "pc_layout.h"

namespace luma::app::pc3d {
using namespace s3d;

inline void Chassis(Scene& sc, int id) {
    const float w = pc::kCaseW / 2, d = pc::kCaseD / 2, h = pc::kCaseH;
    const uint32_t frame = Rgba(45, 51, 63), trim = Rgba(75, 83, 98);
    for (float x : {-w, w}) {
        for (float z : {-d, d})
            model3d::BevelBox(sc, {x - 0.4f, 0, z - 0.5f}, {x + 0.4f, h, z + 0.5f}, 0.18f, frame, id);
        for (float y : {0.7f, h - 0.7f})
            model3d::BevelBox(sc, {x - 0.35f, y - 0.4f, -d}, {x + 0.35f, y + 0.4f, d}, 0.15f, trim, id);
    }
    for (float z : {-d, d})
        for (float y : {0.7f, h - 0.7f})
            model3d::BevelBox(sc, {-w, y - 0.5f, z - 0.35f}, {w, y + 0.5f, z + 0.35f}, 0.15f, trim, id);
    // Vents have gaps instead of a solid panel over the fans.
    for (int i = 0; i < 9; ++i) {
        const float x = -8 + static_cast<float>(i) * 2;
        sc.Box({x - 0.15f, 12, d + 0.05f}, {x + 0.15f, h - 2, d + 0.28f}, frame, id);
    }
    for (int i = 0; i < 18; ++i) {
        const float z = -d + 2 + static_cast<float>(i) * 2.4f;
        sc.Box({-8, h + 0.05f, z}, {8, h + 0.2f, z + 0.22f}, frame, id);
    }
    // Clear glass is a separate translucent surface. It tests depth without hiding the PC.
    sc.Quad({w + 0.05f, 1.4f, -d + 1}, {w + 0.05f, 1.4f, d - 1},
            {w + 0.05f, h - 1.4f, d - 1}, {w + 0.05f, h - 1.4f, -d + 1},
            Rgba(134, 172, 205, 18), id, kDoubleSided | kNoPick);
    // Front power button, USB ports and audio jack on the top rail.
    const auto at = sc.xf;
    sc.xf = Transform::Facing({0, 1, 0}, {5.8f, h + 0.3f, d - 1.5f}).Then(at);
    model3d::Cylinder(sc, 0.45f, 0, 0.2f, Rgba(109, 123, 145), id);
    sc.xf = at;
    for (float x : {-1.8f, 0.f}) sc.Box({x - 0.55f, h + 0.25f, d - 2}, {x + 0.55f, h + 0.32f, d - 1.55f}, Rgba(10, 15, 23), id);
    sc.xf = Transform::Facing({0, 1, 0}, {2.3f, h + 0.3f, d - 1.75f}).Then(at);
    sc.Disc(0.22f, 0, 12, Rgba(10, 15, 23), id);
    // Glass fasteners.
    for (float y : {2.f, h - 2})
        for (float z : {-d + 2, d - 2}) {
            sc.xf = Transform::Facing({1, 0, 0}, {w + 0.1f, y, z}).Then(at);
            model3d::Cylinder(sc, 0.28f, 0, 0.18f, Rgba(106, 115, 131), id, 10);
        }
    sc.xf = at;
    // Rear expansion slot brackets and power socket.
    for (int i = 0; i < 7; ++i) {
        const float y = 12.5f + static_cast<float>(i) * 1.65f;
        sc.Box({-w + 2.5f, y, -d - 0.1f}, {w - 4, y + 0.28f, -d + 0.1f}, trim, id);
    }
    sc.Box({-8, 3, -d - 0.15f}, {-4.5f, 5.2f, -d + 0.05f}, Rgba(12, 16, 23), id);
}

inline void BoardDetails(Scene& sc, int id) {
    const float x = -pc::kCaseW / 2 + 1.1f;
    const auto at = sc.xf;
    for (int i = 0; i < 8; ++i) {
        sc.xf = Transform::Facing({1, 0, 0}, {x, 39.5f, -17.f + static_cast<float>(i) * 0.9f}).Then(at);
        model3d::Cylinder(sc, 0.25f, 0, 0.8f, Rgba(145, 152, 158), id, 10);
    }
    sc.xf = at;
    for (int i = 0; i < 4; ++i) {
        const float y = 15.5f + 2.5f * static_cast<float>(i);
        model3d::BevelBox(sc, {x, y, -16}, {x + 0.5f, y + 0.8f, -5}, 0.1f, Rgba(19, 23, 31), id);
        sc.Box({x + 0.5f, y + 0.3f, -15.5f}, {x + 0.52f, y + 0.45f, -5.5f}, Rgba(97, 112, 124), id);
    }
    for (int i = 0; i < 12; ++i) {
        const float y = 25 + static_cast<float>(i) * 0.65f;
        sc.Box({x, y, 2.4f}, {x + 0.9f, y + 0.45f, 3.3f}, Rgba(43, 49, 59), id);
        sc.Box({x + 0.92f, y + 0.1f, 2.5f}, {x + 0.96f, y + 0.3f, 3.1f}, Rgba(142, 128, 89), id);
    }
    for (float y : {15.f, 42.f})
        for (float z : {-19.f, 2.f}) {
            sc.xf = Transform::Facing({1, 0, 0}, {x, y, z}).Then(at);
            sc.Disc(0.2f, 0.1f, 10, Rgba(157, 164, 174), id);
        }
    sc.xf = at;
}

inline void GpuDetails(Scene& sc, int id) {
    for (int i = 0; i < 16; ++i) {
        const float z = -19 + static_cast<float>(i) * 1.7f;
        sc.Box({-7.5f, 26.41f, z}, {1.8f, 26.47f, z + 0.32f}, Rgba(27, 33, 43), id);
    }
    model3d::BevelBox(sc, {3.65f, 21.7f, -19}, {4.1f, 25.5f, 8}, 0.18f, Rgba(59, 66, 81), id);
    // Braided power lead and its socket run down to the shroud.
    model3d::BevelBox(sc, {3.7f, 24.2f, 5}, {5.4f, 25.3f, 7}, 0.15f, Rgba(22, 27, 35), id);
    for (int i = 0; i < 3; ++i) {
        const float z = 5.3f + static_cast<float>(i) * 0.5f;
        model3d::Tube(sc, {5.4f, 24.6f, z}, {7.2f, 21.8f, z}, 0.15f, Rgba(26, 31, 40), id);
        model3d::Tube(sc, {7.2f, 21.8f, z}, {7.2f, 10.2f, z}, 0.15f, Rgba(26, 31, 40), id);
    }
}
}  // namespace luma::app::pc3d
