// Your monitors as Windows has arranged them (Settings > Display), placed on the desk for the
// 3D views: side by side where they touch in Windows' layout, one above the other where one
// sits on top, each its real size. Pure C++, tested; the Windows side is in displays.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace luma::app::displays {

// One active display, as Windows reports it.
struct Display {
    std::string name;             // "DELL S2721DGF", or the adapter's "\\.\DISPLAY1"
    int x = 0, y = 0;             // its top-left corner on Windows' desktop (pixels)
    int w = 1920, h = 1080;       // its size there (pixels)
    int hz = 0;                   // refresh rate (0: unknown)
    float widthCm = 0, heightCm = 0;  // its screen's physical size (0: unknown)
    bool primary = false;
    std::string id;               // Windows monitor device path, independent of display numbering
    float diagonalInches = 0;     // saved correction (0: use the reported size)
};

inline bool ValidDiagonal(float inches) { return std::isfinite(inches) && inches >= 3 && inches <= 150; }

// Hex keeps Windows device paths (and non-ASCII names) safe inside an INI value.
inline std::string SizeKey(const Display& d) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : d.id.empty() ? d.name : d.id) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

inline std::map<std::string, float> DecodeSizes(const std::string& text) {
    std::map<std::string, float> out;
    for (size_t pos = 0; pos < text.size();) {
        const size_t bar = text.find('|', pos);
        const std::string item = text.substr(pos, bar - pos);
        const size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0 && eq % 2 == 0 &&
            item.substr(0, eq).find_first_not_of("0123456789ABCDEF") == std::string::npos) {
            const std::string value = item.substr(eq + 1);
            char* end = nullptr;
            const float inches = std::strtof(value.c_str(), &end);
            if (end != value.c_str() && *end == '\0' && ValidDiagonal(inches)) out[item.substr(0, eq)] = inches;
        }
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    return out;
}

inline std::string EncodeSizes(const std::map<std::string, float>& sizes) {
    std::string out;
    for (const auto& [key, inches] : sizes) {
        if (key.empty() || !ValidDiagonal(inches)) continue;
        char value[32];
        std::snprintf(value, sizeof value, "%.4g", inches);
        out += (out.empty() ? "" : "|") + key + "=" + value;
    }
    return out;
}

inline void ApplySizes(std::vector<Display>* list, const std::map<std::string, float>& sizes) {
    for (Display& d : *list) {
        const auto it = sizes.find(SizeKey(d));
        d.diagonalInches = it != sizes.end() && ValidDiagonal(it->second) ? it->second : 0;
    }
}

// Where a display stands, in the monitors' own frame on the desk (cm): its center across
// (x, right is +), how far it's moved towards you (z), the height of its screen's bottom edge
// above the desk, its screen's size, and how far it's turned towards you (radians, counter-
// clockwise seen from above).
struct Placed {
    float x = 0, z = 0, bottom = 12;
    float w = 60, h = 34;
    float yaw = 0;
    bool resting = false;  // a small screen (under 16"): no stand, resting on the desk, leaning back
    float lean = 0;        // how far it leans back (radians)
};

// A small screen (under 16"): a sensor panel, a portable display.
inline bool Small(const Placed& p) { return std::sqrt(p.w * p.w + p.h * p.h) < 16 * 2.54f; }

// The screen's size: as reported, else a guess from its resolution (34" ultrawide, 27" for
// 1440p and up, else 24"), matched to its orientation.
inline void ScreenSize(const Display& d, float* w, float* h) {
    if (ValidDiagonal(d.diagonalInches)) {
        const float aspect = static_cast<float>(std::max(1, d.w)) / static_cast<float>(std::max(1, d.h));
        *h = d.diagonalInches * 2.54f / std::sqrt(1 + aspect * aspect);
        *w = *h * aspect;
        return;
    }
    float cw = d.widthCm, ch = d.heightCm;
    if (!std::isfinite(cw) || !std::isfinite(ch) || cw < 15 || cw > 250 || ch < 8 || ch > 150) {
        const float aspect = static_cast<float>(std::max(d.w, d.h)) / static_cast<float>(std::max(1, std::min(d.w, d.h)));
        const int longer = std::max(d.w, d.h);
        const float inches = aspect > 2.f ? 34.f : longer >= 2560 ? 27.f : 24.f;
        const float diag = inches * 2.54f;
        cw = diag * aspect / std::sqrt(1 + aspect * aspect);
        ch = cw / aspect;
    }
    const bool portrait = d.h > d.w;
    if (portrait != (ch > cw)) std::swap(cw, ch);
    *w = cw;
    *h = ch;
}

inline std::vector<Placed> Arrange(const std::vector<Display>& list) {
    const int n = static_cast<int>(list.size());
    std::vector<Placed> out(static_cast<size_t>(n));
    if (!n) return out;
    int prim = 0;
    for (int i = 0; i < n; ++i)
        if (list[static_cast<size_t>(i)].primary) prim = i;
    for (int i = 0; i < n; ++i) ScreenSize(list[static_cast<size_t>(i)], &out[static_cast<size_t>(i)].w, &out[static_cast<size_t>(i)].h);
    const Display& p = list[static_cast<size_t>(prim)];
    const float cmPerPx = out[static_cast<size_t>(prim)].w / static_cast<float>(std::max(1, p.w));
    // Centers (x across, y up), first straight from Windows' pixels...
    std::vector<float> cx(static_cast<size_t>(n)), cy(static_cast<size_t>(n));
    auto centerX = [&](const Display& d) { return static_cast<float>(d.x) + static_cast<float>(d.w) / 2; };
    auto centerY = [&](const Display& d) { return static_cast<float>(d.y) + static_cast<float>(d.h) / 2; };
    for (int i = 0; i < n; ++i) {
        const Display& d = list[static_cast<size_t>(i)];
        cx[static_cast<size_t>(i)] = (centerX(d) - centerX(p)) * cmPerPx;
        cy[static_cast<size_t>(i)] = -(centerY(d) - centerY(p)) * cmPerPx;
    }
    // ...then, outwards from the primary, displays that touch in Windows' layout get their
    // neighbor's edge (with a bezel's gap), whatever their resolution.
    constexpr float kGap = 1.5f;
    std::vector<bool> placed(static_cast<size_t>(n), false);
    std::vector<int> queue{prim};
    placed[static_cast<size_t>(prim)] = true;
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const int a = queue[qi];
        const Display& da = list[static_cast<size_t>(a)];
        for (int b = 0; b < n; ++b) {
            if (placed[static_cast<size_t>(b)]) continue;
            const Display& db = list[static_cast<size_t>(b)];
            const bool rowOverlap = db.y < da.y + da.h && da.y < db.y + db.h;
            const bool colOverlap = db.x < da.x + da.w && da.x < db.x + db.w;
            const Placed &pa = out[static_cast<size_t>(a)], &pb = out[static_cast<size_t>(b)];
            const float rowShift = -(centerY(db) - centerY(da)) / static_cast<float>(std::max(1, da.h)) * pa.h;
            const float colShift = (centerX(db) - centerX(da)) / static_cast<float>(std::max(1, da.w)) * pa.w;
            if (rowOverlap && std::abs(db.x - (da.x + da.w)) <= 2) {  // to the right
                cx[static_cast<size_t>(b)] = cx[static_cast<size_t>(a)] + (pa.w + pb.w) / 2 + kGap;
                cy[static_cast<size_t>(b)] = cy[static_cast<size_t>(a)] + rowShift;
            } else if (rowOverlap && std::abs(da.x - (db.x + db.w)) <= 2) {  // to the left
                cx[static_cast<size_t>(b)] = cx[static_cast<size_t>(a)] - (pa.w + pb.w) / 2 - kGap;
                cy[static_cast<size_t>(b)] = cy[static_cast<size_t>(a)] + rowShift;
            } else if (colOverlap && std::abs(db.y - (da.y + da.h)) <= 2) {  // below
                // A small screen under a monitor sits right under its bottom edge.
                const float gap = Small(pb) && !Small(pa) ? 0.3f : kGap;
                cy[static_cast<size_t>(b)] = cy[static_cast<size_t>(a)] - (pa.h + pb.h) / 2 - gap;
                cx[static_cast<size_t>(b)] = cx[static_cast<size_t>(a)] + colShift;
            } else if (colOverlap && std::abs(da.y - (db.y + db.h)) <= 2) {  // above
                cy[static_cast<size_t>(b)] = cy[static_cast<size_t>(a)] + (pa.h + pb.h) / 2 + kGap;
                cx[static_cast<size_t>(b)] = cx[static_cast<size_t>(a)] + colShift;
            } else {
                continue;
            }
            placed[static_cast<size_t>(b)] = true;
            queue.push_back(b);
        }
    }
    // A small screen right under a bigger one (in Windows' layout) stands on the desk beneath
    // it; the one above sits on top of it. Other small screens rest on the desk in front.
    std::vector<int> under(static_cast<size_t>(n), -1);
    for (int i = 0; i < n; ++i) {
        if (!Small(out[static_cast<size_t>(i)])) continue;
        const Display& di = list[static_cast<size_t>(i)];
        for (int j = 0; j < n && under[static_cast<size_t>(i)] < 0; ++j) {
            const Display& dj = list[static_cast<size_t>(j)];
            if (j != i && !Small(out[static_cast<size_t>(j)]) && di.x < dj.x + dj.w && dj.x < di.x + di.w &&
                std::abs(di.y - (dj.y + dj.h)) <= 2)
                under[static_cast<size_t>(i)] = j;
        }
    }
    auto loose = [&](int i) { return Small(out[static_cast<size_t>(i)]) && under[static_cast<size_t>(i)] < 0; };
    // Heights: the lowest screen's bottom edge 12 cm above the desk (on its stand), or on the
    // desk if that's a small screen under another.
    float lowest = 1e9f;
    int lowestAt = -1;
    for (int i = 0; i < n; ++i) {
        if (loose(i)) continue;
        const float b = cy[static_cast<size_t>(i)] - out[static_cast<size_t>(i)].h / 2;
        if (b < lowest) lowest = b, lowestAt = i;
    }
    const float base = lowestAt >= 0 && under[static_cast<size_t>(lowestAt)] >= 0 ? 0.3f : 12.f;
    const Placed& pp = out[static_cast<size_t>(prim)];
    for (int i = 0; i < n; ++i) {
        Placed& o = out[static_cast<size_t>(i)];
        o.x = cx[static_cast<size_t>(i)];
        if (loose(i)) {
            o.resting = true;
            o.bottom = 0.3f;
            o.z = 16;
            o.lean = 0.26f;
            continue;
        }
        o.bottom = base + (cy[static_cast<size_t>(i)] - o.h / 2 - lowest);
        if (Small(o)) continue;  // placed with the one above it, below
        // Beside the primary (in its row): turned towards you and a little forward.
        const bool beside = std::fabs(o.x) > pp.w / 2 && std::fabs(cy[static_cast<size_t>(i)] - cy[static_cast<size_t>(prim)]) < (o.h + pp.h) / 2;
        if (beside) {
            o.yaw = o.x > 0 ? -0.35f : 0.35f;
            o.z = (std::fabs(o.x) - pp.w / 2) * 0.35f;
        }
    }
    for (int i = 0; i < n; ++i) {
        const int j = under[static_cast<size_t>(i)];
        if (j < 0) continue;
        Placed& o = out[static_cast<size_t>(i)];
        const Placed& above = out[static_cast<size_t>(j)];
        o.yaw = above.yaw;
        o.z = above.z + 6.5f;  // in front of the stand's foot
        if (o.bottom < 2) {    // on the desk, leaning a little against the stand
            o.resting = true;
            o.bottom = 0.3f;
            o.lean = 0.1f;
        }
    }
    return out;
}

}  // namespace luma::app::displays
