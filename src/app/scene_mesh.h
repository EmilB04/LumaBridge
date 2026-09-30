// Triangles for the D3D11 setup view. Keep projection and mesh conversion portable so the
// depth behavior can be checked without a graphics device.
#pragma once

#include <utility>

#include "scene3d.h"

namespace luma::app::s3d {

struct MeshVertex {
    float x, y, z, w;  // homogeneous clip coordinates, w = view depth
    uint32_t color;
    float u = 0, v = 0, glow = 0;
};

struct Mesh {
    std::vector<MeshVertex> opaque, transparent;
};

inline MeshVertex MeshPoint(float x, float y, float depth, uint32_t color, const Viewport& vp) {
    const float z = std::max(kNear, depth);
    return {((x - vp.x) / vp.w * 2 - 1) * z, (1 - (y - vp.y) / vp.h * 2) * z,
            z - kNear, z, color};
}

inline Mesh MakeMesh(const std::vector<DrawItem>& items, const Viewport& vp, float scale = 1) {
    Mesh mesh;
    if (vp.w <= 0 || vp.h <= 0) return mesh;
    mesh.opaque.reserve(items.size() * 9);
    struct Part { std::vector<MeshVertex> vertices; float depth; };
    std::vector<Part> transparent;
    auto quad = [](std::vector<MeshVertex>& out, const std::array<MeshVertex, 4>& v) {
        for (int i : {0, 1, 2, 0, 2, 3}) out.push_back(v[static_cast<size_t>(i)]);
    };
    // An opaque gradient prevents the resolved MSAA edges from being blended twice by ImGui.
    quad(mesh.opaque, {{{-1, 1, 1, 1, Rgba(20, 26, 38)}, {1, 1, 1, 1, Rgba(20, 26, 38)},
                         {1, -1, 1, 1, Rgba(7, 9, 13)}, {-1, -1, 1, 1, Rgba(7, 9, 13)}}});
    for (const DrawItem& item : items) {
        if ((item.color >> 24) == 0) continue;
        const bool solid = item.kind == DrawItem::Polygon && (item.color >> 24) == 255;
        Part part;
        auto& out = solid ? mesh.opaque : part.vertices;
        if (item.kind == DrawItem::Polygon) {
            if (item.n < 3) continue;
            part.depth = 0;
            for (int i = 0; i < item.n; ++i) part.depth += item.vertexDepth[i];
            part.depth /= static_cast<float>(item.n);
            auto point = [&](int i) { return MeshPoint(item.xy[i * 2], item.xy[i * 2 + 1], item.vertexDepth[i], item.color, vp); };
            for (int i = 1; i + 1 < item.n; ++i) {
                out.push_back(point(0)); out.push_back(point(i)); out.push_back(point(i + 1));
            }
        } else if (item.kind == DrawItem::Segment) {
            const float dx = item.xy[2] - item.xy[0], dy = item.xy[3] - item.xy[1];
            const float length = std::hypot(dx, dy);
            if (length < 0.01f) continue;
            const float ox = -dy / length * item.width * scale * 0.5f, oy = dx / length * item.width * scale * 0.5f;
            // Only a small offset for an edge on its own surface; it cannot jump through a case.
            const float a = std::max(kNear, item.vertexDepth[0] - 0.015f), b = std::max(kNear, item.vertexDepth[1] - 0.015f);
            quad(out, {{MeshPoint(item.xy[0] + ox, item.xy[1] + oy, a, item.color, vp),
                         MeshPoint(item.xy[2] + ox, item.xy[3] + oy, b, item.color, vp),
                         MeshPoint(item.xy[2] - ox, item.xy[3] - oy, b, item.color, vp),
                         MeshPoint(item.xy[0] - ox, item.xy[1] - oy, a, item.color, vp)}});
            part.depth = (a + b) / 2;
        } else {
            const float r = std::max(1.f, item.radius), z = item.vertexDepth[0];
            std::array<MeshVertex, 4> v;
            constexpr float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (int i = 0; i < 4; ++i) {
                v[static_cast<size_t>(i)] = MeshPoint(item.xy[0] + corners[i][0] * r, item.xy[1] + corners[i][1] * r,
                                                     z, item.color, vp);
                v[static_cast<size_t>(i)].u = corners[i][0];
                v[static_cast<size_t>(i)].v = corners[i][1];
                v[static_cast<size_t>(i)].glow = 1;
            }
            quad(out, v);
            part.depth = z;
        }
        if (!solid && !part.vertices.empty()) transparent.push_back(std::move(part));
    }
    // Glass, airflow and glow blend after opaque surfaces, while still testing their depth.
    std::stable_sort(transparent.begin(), transparent.end(), [](const Part& a, const Part& b) { return a.depth > b.depth; });
    for (const Part& part : transparent) mesh.transparent.insert(mesh.transparent.end(), part.vertices.begin(), part.vertices.end());
    return mesh;
}

}  // namespace luma::app::s3d
