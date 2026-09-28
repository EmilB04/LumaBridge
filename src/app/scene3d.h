// A small software 3D renderer for the setup views: an orbit camera, flat-shaded convex faces
// sorted back to front (painter's algorithm), glowing points for LEDs, and lines. It only
// computes 2D polygons; the UI draws them with Dear ImGui's draw list. Pure C++, tested.
//
// Units are centimetres; y is up. Faces are one-sided (their front is where the corners run
// counter-clockwise, seen from outside) unless marked double-sided, so a case built from
// inward-facing walls always shows its inside: the wall between you and it is culled.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace luma::app::s3d {

struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Normalize(V3 a) {
    const float l = Length(a);
    return l > 1e-6f ? a * (1.f / l) : V3{0, 0, 0};
}
inline V3 Lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }

// Packed like Dear ImGui's IM_COL32: red in the low byte, alpha in the high one.
inline uint32_t Rgba(int r, int g, int b, int a = 255) {
    auto c = [](int v) { return static_cast<uint32_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
    return c(r) | c(g) << 8 | c(b) << 16 | c(a) << 24;
}
inline uint32_t ScaleRgb(uint32_t c, float k) {
    return Rgba(static_cast<int>((c & 0xFF) * k), static_cast<int>((c >> 8 & 0xFF) * k),
                static_cast<int>((c >> 16 & 0xFF) * k), static_cast<int>(c >> 24));
}
inline uint32_t WithAlpha(uint32_t c, int a) { return (c & 0x00FFFFFFu) | static_cast<uint32_t>(a < 0 ? 0 : a > 255 ? 255 : a) << 24; }

// ---- Camera -------------------------------------------------------------------------------

// Orbits `target`: yaw turns around the vertical axis (0 = looking along -z from +z), pitch
// looks down from above (radians), `distance` from the target.
struct Camera {
    V3 target{0, 20, 0};
    float yaw = 0.4f, pitch = 0.45f, distance = 180.f;
    float zoom = 1.f;  // focal length as a fraction of the viewport's smaller side

    V3 Eye() const {
        const float cp = std::cos(pitch);
        return target + V3{std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp} * distance;
    }
    // Right, up and forward (from the eye to the target) unit vectors.
    void Basis(V3* right, V3* up, V3* forward) const {
        const V3 f = Normalize(target - Eye());
        V3 r = Normalize(Cross(f, V3{0, 1, 0}));
        if (Length(r) < 0.5f) r = {1, 0, 0};
        *forward = f;
        *right = r;
        *up = Cross(r, f);
    }
    void Clamp() {
        pitch = std::clamp(pitch, 0.05f, 1.45f);
        distance = std::clamp(distance, 30.f, 600.f);
    }
};

struct Viewport {
    float x = 0, y = 0, w = 100, h = 100;
    float Focal(const Camera& c) const { return std::min(w, h) * 1.2f * c.zoom; }
};

// A point in view space (x right, y up, z forward) and on screen.
struct Projected {
    float sx = 0, sy = 0, z = 0;
    bool visible = false;  // in front of the camera
};

constexpr float kNear = 1.f;

inline Projected Project(const Camera& cam, const Viewport& vp, V3 p) {
    V3 r, u, f;
    cam.Basis(&r, &u, &f);
    const V3 d = p - cam.Eye();
    Projected out;
    out.z = Dot(d, f);
    if (out.z < kNear) return out;
    const float k = vp.Focal(cam) / out.z;
    out.sx = vp.x + vp.w * 0.5f + Dot(d, r) * k;
    out.sy = vp.y + vp.h * 0.5f - Dot(d, u) * k;
    out.visible = true;
    return out;
}

struct Ray {
    V3 origin, dir;
};

// The ray from the eye through a point on the screen.
inline Ray ScreenRay(const Camera& cam, const Viewport& vp, float sx, float sy) {
    V3 r, u, f;
    cam.Basis(&r, &u, &f);
    const float k = 1.f / vp.Focal(cam);
    const float dx = (sx - vp.x - vp.w * 0.5f) * k, dy = -(sy - vp.y - vp.h * 0.5f) * k;
    return {cam.Eye(), Normalize(f + r * dx + u * dy)};
}

// Where the ray meets the horizontal plane at height y (false if it doesn't, going away).
inline bool HitPlaneY(const Ray& ray, float y, V3* hit) {
    if (std::fabs(ray.dir.y) < 1e-5f) return false;
    const float t = (y - ray.origin.y) / ray.dir.y;
    if (t <= 0) return false;
    *hit = ray.origin + ray.dir * t;
    return true;
}

// ---- Scene --------------------------------------------------------------------------------

enum FaceFlags : uint32_t {
    kEmissive = 1,      // lit from within (LEDs): no shading
    kDoubleSided = 2,   // drawn from both sides (fan blades, thin plates)
    kBackground = 4,    // walls and floors: drawn before everything else
    kNoPick = 8,        // never picked (airflow, glass)
};

constexpr int kMaxCorners = 8;

struct Face {
    std::array<V3, kMaxCorners> p{};
    int n = 0;
    uint32_t color = 0;
    uint32_t flags = 0;
    int id = -1;  // what it belongs to (for picking); -1 nothing
};

struct Glow {
    V3 at;
    float radius = 1;  // world units
    uint32_t color = 0;
};

struct Line {
    V3 a, b;
    uint32_t color = 0;
    float width = 1;
};

// A rigid placement: rotation (columns = where the local x, y, z axes point) and position.
struct Transform {
    V3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1}, origin{};
    V3 Apply(V3 p) const { return origin + x * p.x + y * p.y + z * p.z; }
    V3 Rotate(V3 v) const { return x * v.x + y * v.y + z * v.z; }
    // This placement inside `parent`.
    Transform Then(const Transform& parent) const {
        return {parent.Rotate(x), parent.Rotate(y), parent.Rotate(z), parent.Apply(origin)};
    }
    static Transform Translate(V3 at) {
        Transform t;
        t.origin = at;
        return t;
    }
    // Turned `radians` around the vertical axis (counter-clockwise seen from above), at `at`.
    static Transform YawAt(float radians, V3 at) {
        Transform t;
        const float c = std::cos(radians), s = std::sin(radians);
        t.x = {c, 0, -s};
        t.z = {s, 0, c};
        t.origin = at;
        return t;
    }
    // A frame whose local z points along `normal` (for fans and discs), at `at`.
    static Transform Facing(V3 normal, V3 at) {
        Transform t;
        t.z = Normalize(normal);
        const V3 helper = std::fabs(t.z.y) > 0.9f ? V3{1, 0, 0} : V3{0, 1, 0};
        t.x = Normalize(Cross(helper, t.z));
        t.y = Cross(t.z, t.x);
        t.origin = at;
        return t;
    }
};

class Scene {
public:
    std::vector<Face> faces;
    std::vector<Glow> glows;
    std::vector<Line> lines;
    Transform xf;  // applied to everything added

    void Poly(const V3* pts, int n, uint32_t color, int id, uint32_t flags = 0) {
        Face f;
        f.n = std::min(n, kMaxCorners);
        for (int i = 0; i < f.n; ++i) f.p[static_cast<size_t>(i)] = xf.Apply(pts[i]);
        f.color = color;
        f.flags = flags;
        f.id = id;
        faces.push_back(f);
    }
    void Quad(V3 a, V3 b, V3 c, V3 d, uint32_t color, int id, uint32_t flags = 0) {
        const V3 pts[] = {a, b, c, d};
        Poly(pts, 4, color, id, flags);
    }
    // An axis-aligned box (in the current placement): `lo` to `hi`, faces pointing out.
    void Box(V3 lo, V3 hi, uint32_t color, int id, uint32_t flags = 0) {
        const V3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                         {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
        Quad(c[4], c[5], c[6], c[7], color, id, flags);  // +z
        Quad(c[1], c[0], c[3], c[2], color, id, flags);  // -z
        Quad(c[5], c[1], c[2], c[6], color, id, flags);  // +x
        Quad(c[0], c[4], c[7], c[3], color, id, flags);  // -x
        Quad(c[3], c[7], c[6], c[2], color, id, flags);  // +y
        Quad(c[0], c[1], c[5], c[4], color, id, flags);  // -y
    }
    // The inside of a box: its six walls facing inwards (see the file comment).
    void Room(V3 lo, V3 hi, uint32_t color, int id, uint32_t flags = kBackground) {
        const V3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                         {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
        Quad(c[7], c[6], c[5], c[4], color, id, flags);
        Quad(c[2], c[3], c[0], c[1], color, id, flags);
        Quad(c[6], c[2], c[1], c[5], color, id, flags);
        Quad(c[3], c[7], c[4], c[0], color, id, flags);
        Quad(c[2], c[6], c[7], c[3], color, id, flags);
        Quad(c[4], c[5], c[1], c[0], color, id, flags);
    }
    // A flat disc of radius r in the local xy plane at z, facing +z.
    void Disc(float r, float z, int sides, uint32_t color, int id, uint32_t flags = 0) {
        V3 pts[kMaxCorners];
        sides = std::clamp(sides, 3, kMaxCorners);
        for (int i = 0; i < sides; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(sides);
            pts[i] = {std::cos(a) * r, std::sin(a) * r, z};
        }
        Poly(pts, sides, color, id, flags);
    }
    void AddGlow(V3 at, float radius, uint32_t color) { glows.push_back({xf.Apply(at), radius, color}); }
    void AddLine(V3 a, V3 b, uint32_t color, float width = 1) { lines.push_back({xf.Apply(a), xf.Apply(b), color, width}); }
    // The twelve edges of a box.
    void Edges(V3 lo, V3 hi, uint32_t color, float width = 1) {
        const V3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                         {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
        const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& k : e) AddLine(c[k[0]], c[k[1]], color, width);
    }
};

// ---- Rendering to 2D ----------------------------------------------------------------------

struct DrawItem {
    enum Kind : uint8_t { Polygon, GlowDot, Segment } kind = Polygon;
    float xy[kMaxCorners * 2] = {};
    int n = 0;
    uint32_t color = 0;
    float depth = 0;  // distance from the eye; larger is further
    float radius = 0; // glow: pixels
    float width = 1;  // segment: pixels
    int id = -1;
    bool background = false;
    bool pickable = false;
};

// Everything the scene shows, far to near, ready to draw. Shading: faces turned towards
// `light` are brighter (ambient 0.5).
inline std::vector<DrawItem> Render(const Scene& scene, const Camera& cam, const Viewport& vp,
                                    V3 light = Normalize(V3{0.35f, 1.f, 0.55f})) {
    std::vector<DrawItem> out;
    out.reserve(scene.faces.size() + scene.glows.size() + scene.lines.size());
    const V3 eye = cam.Eye();
    for (const Face& f : scene.faces) {
        if (f.n < 3) continue;
        V3 normal = Normalize(Cross(f.p[1] - f.p[0], f.p[2] - f.p[0]));
        const bool facing = Dot(normal, eye - f.p[0]) > 0;
        if (!facing && !(f.flags & kDoubleSided)) continue;
        if (!facing) normal = normal * -1.f;
        DrawItem d;
        d.kind = DrawItem::Polygon;
        d.n = f.n;
        bool ok = true;
        float z = 0;
        for (int i = 0; i < f.n && ok; ++i) {
            const Projected p = Project(cam, vp, f.p[static_cast<size_t>(i)]);
            ok = p.visible;
            d.xy[i * 2] = p.sx;
            d.xy[i * 2 + 1] = p.sy;
            z += p.z;
        }
        if (!ok) continue;
        d.depth = z / static_cast<float>(f.n);
        if (f.flags & kEmissive) {
            d.color = f.color;
        } else {
            const float lit = 0.5f + 0.5f * std::max(0.f, Dot(normal, light));
            d.color = ScaleRgb(f.color, lit);
        }
        d.id = f.id;
        d.background = (f.flags & kBackground) != 0;
        d.pickable = f.id >= 0 && !(f.flags & kNoPick);
        out.push_back(d);
    }
    const float focal = vp.Focal(cam);
    for (const Glow& g : scene.glows) {
        const Projected p = Project(cam, vp, g.at);
        if (!p.visible) continue;
        DrawItem d;
        d.kind = DrawItem::GlowDot;
        d.xy[0] = p.sx;
        d.xy[1] = p.sy;
        d.radius = g.radius * focal / p.z;
        d.depth = p.z - 0.5f;  // just in front of what it sits on
        d.color = g.color;
        out.push_back(d);
    }
    for (const Line& l : scene.lines) {
        const Projected a = Project(cam, vp, l.a), b = Project(cam, vp, l.b);
        if (!a.visible || !b.visible) continue;
        DrawItem d;
        d.kind = DrawItem::Segment;
        d.xy[0] = a.sx;
        d.xy[1] = a.sy;
        d.xy[2] = b.sx;
        d.xy[3] = b.sy;
        d.n = 2;
        d.depth = std::min(a.z, b.z) - 1.f;  // outlines sit on top of the faces they border
        d.color = l.color;
        d.width = l.width;
        out.push_back(d);
    }
    std::stable_sort(out.begin(), out.end(), [](const DrawItem& a, const DrawItem& b) {
        if (a.background != b.background) return a.background;  // walls and floors first
        return a.depth > b.depth;
    });
    return out;
}

// Whether the point is inside a convex polygon (either winding).
inline bool InsidePolygon(const float* xy, int n, float px, float py) {
    bool pos = false, neg = false;
    for (int i = 0; i < n; ++i) {
        const float ax = xy[i * 2], ay = xy[i * 2 + 1];
        const float bx = xy[((i + 1) % n) * 2], by = xy[((i + 1) % n) * 2 + 1];
        const float c = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
        pos |= c > 0;
        neg |= c < 0;
        if (pos && neg) return false;
    }
    return true;
}

// The id of the nearest pickable face under the point, or -1.
inline int Pick(const std::vector<DrawItem>& items, float px, float py) {
    for (auto it = items.rbegin(); it != items.rend(); ++it)
        if (it->kind == DrawItem::Polygon && it->pickable && InsidePolygon(it->xy, it->n, px, py)) return it->id;
    return -1;
}

}  // namespace luma::app::s3d
