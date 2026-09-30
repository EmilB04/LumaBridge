// What sits where in the PC case, for the 3D views: which fan slots hold a fan (and whether it
// is one of the RGB fans on the ARGB header), which way each position blows, and how the case
// is turned on the desk. LumaBridge guesses it from what it found; you correct it on the My
// setup page. Pure C++, tested.
//
// The case, in its own coordinates (cm): x from the motherboard (-) to the glass side (+),
// y up from the floor, z from the back (-) to the front (+). Slots, in the order fans are
// guessed into: front (top to bottom), back, top (front to back), bottom (on the PSU shroud).
#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "scene3d.h"

namespace luma::app::pc {

// The case's size (a typical mid tower).
constexpr float kCaseW = 22, kCaseH = 47, kCaseD = 45;
constexpr float kShroudH = 10;  // the PSU shroud along the bottom

enum class Mount { Front = 0, Back = 1, Top = 2, Bottom = 3 };
constexpr int kMounts = 4;
inline const char* MountName(Mount m) {
    switch (m) {
    case Mount::Front: return "Front";
    case Mount::Back: return "Back";
    case Mount::Top: return "Top";
    default: return "Bottom";
    }
}

struct Slot {
    Mount mount;
    s3d::V3 center;  // case coordinates
    s3d::V3 inward;  // pointing into the case
    float radius;    // the fan's (a 120 mm fan: 6)
};

constexpr int kSlots = 9;
inline const std::array<Slot, kSlots>& Slots() {
    static const std::array<Slot, kSlots> kAll = {{
        {Mount::Front, {0, 40.5f, 20.5f}, {0, 0, -1}, 5.8f},
        {Mount::Front, {0, 28.5f, 20.5f}, {0, 0, -1}, 5.8f},
        {Mount::Front, {0, 16.5f, 20.5f}, {0, 0, -1}, 5.8f},
        {Mount::Back, {0.5f, 36, -20.5f}, {0, 0, 1}, 5.8f},
        {Mount::Top, {2, 45, 12.5f}, {0, -1, 0}, 5.8f},
        {Mount::Top, {2, 45, 0.5f}, {0, -1, 0}, 5.8f},
        {Mount::Top, {2, 45, -11.5f}, {0, -1, 0}, 5.8f},
        {Mount::Bottom, {3, 10.5f, 12}, {0, 1, 0}, 5.8f},
        {Mount::Bottom, {3, 10.5f, 0}, {0, 1, 0}, 5.8f},
    }};
    return kAll;
}

enum class SlotFan : uint8_t { None = 0, Plain = 1, Rgb = 2 };

// The CPU cooler: a tower heatsink with a fan, or an all-in-one water cooler (a pump on the
// CPU, tubes, and a radiator behind the top or front fans).
enum class Cooler : uint8_t { Air = 0, Aio = 1 };

struct Layout {
    std::array<SlotFan, kSlots> slots{};
    // Per mount: true when its fans blow out of the case. Front and bottom pull air in, back
    // and top push it out: the usual way.
    std::array<bool, kMounts> exhaust{false, true, true, false};
    int turn = 0;           // quarter turns on the desk (0: its front towards you, the glass on the right)
    Cooler cooler = Cooler::Air;
    Mount radiator = Mount::Top;  // an AIO's radiator: Top or Front
    bool pumpRgb = false;         // the AIO's pump head is lit, chained on the ARGB header
    bool guessed = true;    // LumaBridge's guess, not yet corrected

    int Fans() const {
        int n = 0;
        for (SlotFan s : slots) n += s != SlotFan::None;
        return n;
    }
    int RgbFans() const {
        int n = 0;
        for (SlotFan s : slots) n += s == SlotFan::Rgb;
        return n;
    }
    bool Exhaust(int slot) const { return exhaust[static_cast<size_t>(Slots()[static_cast<size_t>(slot)].mount)]; }
    // Where this slot's fan sits on the ARGB chain (0 = first), or -1 if it isn't an RGB fan.
    // The chain runs in slot order.
    int ChainIndex(int slot) const {
        if (slots[static_cast<size_t>(slot)] != SlotFan::Rgb) return -1;
        int k = 0;
        for (int i = 0; i < slot; ++i) k += slots[static_cast<size_t>(i)] == SlotFan::Rgb;
        return k;
    }
    bool operator==(const Layout& o) const {
        return slots == o.slots && exhaust == o.exhaust && turn == o.turn && guessed == o.guessed && cooler == o.cooler &&
               radiator == o.radiator && pumpRgb == o.pumpRgb;
    }
};

// LumaBridge's guess: `rgbFans` on the ARGB header and `plainFans` more, filled in slot order
// (front, back, top, bottom), RGB fans first. 6 fans: 3 in front, 1 at the back, 2 on top.
inline Layout Guess(int rgbFans, int plainFans) {
    Layout l;
    int slot = 0;
    for (int i = 0; i < rgbFans && slot < kSlots; ++i) l.slots[static_cast<size_t>(slot++)] = SlotFan::Rgb;
    for (int i = 0; i < plainFans && slot < kSlots; ++i) l.slots[static_cast<size_t>(slot++)] = SlotFan::Plain;
    return l;
}

// "v1;slots=222111000;exhaust=0110;turn=0;cooler=air" (cooler: air, aio-top or aio-front, with
// "-rgb" when the pump head is lit; "" or anything unreadable: guess).
inline std::string Encode(const Layout& l) {
    std::string s = "v1;slots=";
    for (SlotFan f : l.slots) s += static_cast<char>('0' + static_cast<int>(f));
    s += ";exhaust=";
    for (bool e : l.exhaust) s += e ? '1' : '0';
    s += ";turn=" + std::to_string(l.turn);
    s += std::string(";cooler=") + (l.cooler == Cooler::Air ? "air" : l.radiator == Mount::Front ? "aio-front" : "aio-top") +
         (l.cooler == Cooler::Aio && l.pumpRgb ? "-rgb" : "");
    return s;
}

inline bool Decode(const std::string& s, Layout* out) {
    if (s.rfind("v1;", 0) != 0) return false;
    Layout l;
    l.guessed = false;
    auto field = [&](const char* key) -> std::string {
        const std::string k = std::string(key) + "=";
        const size_t at = s.find(k);
        if (at == std::string::npos) return {};
        const size_t end = s.find(';', at);
        return s.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
    };
    const std::string slots = field("slots"), exhaust = field("exhaust"), turn = field("turn");
    if (slots.size() != kSlots || exhaust.size() != kMounts) return false;
    for (int i = 0; i < kSlots; ++i) {
        const int v = slots[static_cast<size_t>(i)] - '0';
        if (v < 0 || v > 2) return false;
        l.slots[static_cast<size_t>(i)] = static_cast<SlotFan>(v);
    }
    for (int i = 0; i < kMounts; ++i) l.exhaust[static_cast<size_t>(i)] = exhaust[static_cast<size_t>(i)] == '1';
    l.turn = turn.empty() ? 0 : ((std::atoi(turn.c_str()) % 4) + 4) % 4;
    const std::string cooler = field("cooler");  // not in layouts saved before AIO coolers: air
    if (cooler.rfind("aio", 0) == 0) {
        l.cooler = Cooler::Aio;
        l.radiator = cooler.rfind("aio-front", 0) == 0 ? Mount::Front : Mount::Top;
        l.pumpRgb = cooler.size() > 4 && cooler.compare(cooler.size() - 4, 4, "-rgb") == 0;
    }
    *out = l;
    return true;
}

// How many fans sit at a mount, and setting it: the first `n` of its slots get a fan (a fan
// already there stays as it is, a new one is plain), the rest are emptied.
inline int FansAt(const Layout& l, Mount m) {
    int n = 0;
    for (int i = 0; i < kSlots; ++i) n += Slots()[static_cast<size_t>(i)].mount == m && l.slots[static_cast<size_t>(i)] != SlotFan::None;
    return n;
}
inline int SlotsAt(Mount m) {
    int n = 0;
    for (const Slot& s : Slots()) n += s.mount == m;
    return n;
}
inline void SetFansAt(Layout* l, Mount m, int n) {
    int k = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (Slots()[static_cast<size_t>(i)].mount != m) continue;
        SlotFan& f = l->slots[static_cast<size_t>(i)];
        if (k++ < n) {
            if (f == SlotFan::None) f = SlotFan::Plain;
        } else {
            f = SlotFan::None;
        }
    }
    l->guessed = false;
}

// Cycles a slot when you click it while editing: empty -> fan -> RGB fan -> empty.
inline void CycleSlot(Layout* l, int slot) {
    SlotFan& f = l->slots[static_cast<size_t>(slot)];
    f = f == SlotFan::None ? SlotFan::Plain : f == SlotFan::Plain ? SlotFan::Rgb : SlotFan::None;
    l->guessed = false;
}

// ---- The desk ------------------------------------------------------------------------------

// The desk (cm): x across (-kDeskW/2 .. +), z from the wall (-) to you (+), its top at y = 0.
constexpr float kDeskW = 170, kDeskD = 84;
constexpr int kMaxOthers = 4;  // other lit devices drawn as light bars

// What can be moved on the desk, as saved (Prefs::setupSpots keys).
inline const std::array<const char*, 10>& DeskItems() {
    static const std::array<const char*, 10> kItems = {"desk:case",     "desk:monitor", "desk:keyboard",
                                                       "desk:mouse",    "desk:headset", "desk:controller",
                                                       "desk:other0",   "desk:other1",  "desk:other2", "desk:other3"};
    return kItems;
}

// Where things go before you move them: the case on the right, the monitor in the middle at
// the back, the keyboard in front of it, the mouse to its right.
inline s3d::V3 DefaultDeskSpot(const std::string& item) {
    if (item == "desk:case") return {52, 0, -14};
    if (item == "desk:monitor") return {-12, 0, -24};
    if (item == "desk:keyboard") return {-14, 0, 12};
    if (item == "desk:mouse") return {22, 0, 14};
    if (item == "desk:headset") return {-62, 0, -6};
    if (item == "desk:controller") return {-38, 0, 30};
    if (item.rfind("desk:other", 0) == 0) return {-58.f + 22.f * static_cast<float>(item.back() - '0'), 0, -36};
    return {};
}

// A desk position as saved (0..1 across and deep) and back.
inline s3d::V3 FromSaved(float sx, float sy) { return {(sx - 0.5f) * kDeskW, 0, (sy - 0.5f) * kDeskD}; }
inline void ToSaved(s3d::V3 p, float* sx, float* sy) {
    *sx = std::clamp(p.x / kDeskW + 0.5f, 0.f, 1.f);
    *sy = std::clamp(p.z / kDeskD + 0.5f, 0.f, 1.f);
}

// ---- Airflow ------------------------------------------------------------------------------

// A particle's position (case coordinates) along its path from an intake fan through the case
// to an exhaust, at `phase` 0..1 (outside the intake at 0, outside the exhaust at 1). `seed`
// spreads particles across the fans' faces. Without intake fans air comes in through the
// front; without exhausts it leaves at the back.
inline s3d::V3 AirflowPoint(const Layout& l, int intakeIndex, int exhaustIndex, float phase, uint32_t seed) {
    using s3d::V3;
    int intakes[kSlots], exhausts[kSlots], ni = 0, ne = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (l.slots[static_cast<size_t>(i)] == SlotFan::None) continue;
        (l.Exhaust(i) ? exhausts[ne++] : intakes[ni++]) = i;
    }
    const auto& S = Slots();
    auto spread = [&](uint32_t k) {
        const uint32_t h = (seed * 2654435761u) ^ (k * 40503u);
        return static_cast<float>(h % 1000u) / 1000.f - 0.5f;
    };
    V3 in, inDir, out, outDir;
    if (ni) {
        const Slot& s = S[static_cast<size_t>(intakes[intakeIndex % ni])];
        in = s.center;
        inDir = s.inward;
    } else {
        in = {0, 28, kCaseD / 2};
        inDir = {0, 0, -1};
    }
    if (ne) {
        const Slot& s = S[static_cast<size_t>(exhausts[exhaustIndex % ne])];
        out = s.center;
        outDir = s.inward * -1.f;
    } else {
        out = {0.5f, 36, -kCaseD / 2};
        outDir = {0, 0, -1};
    }
    // Spread over the fan's face (the two directions across it).
    auto across = [](V3 n, V3* a, V3* b) {
        *a = std::fabs(n.y) > 0.9f ? V3{1, 0, 0} : V3{0, 1, 0};
        *b = s3d::Cross(n, *a);
    };
    V3 a1, b1, a2, b2;
    across(inDir, &a1, &b1);
    across(outDir, &a2, &b2);
    const V3 p0 = in - inDir * 4.f + a1 * (spread(1) * 9) + b1 * (spread(2) * 9);
    const V3 p3 = out + outDir * 6.f + a2 * (spread(3) * 8) + b2 * (spread(4) * 8);
    // Bent through the middle of the case, clear of the shroud.
    const V3 mid{2.f + spread(5) * 8, 30.f + spread(6) * 10, spread(7) * 14};
    const V3 p1 = s3d::Lerp(in + inDir * 8.f, mid, 0.6f), p2 = s3d::Lerp(out - outDir * 8.f, mid, 0.6f);
    const float t = phase < 0 ? 0 : phase > 1 ? 1 : phase, u = 1 - t;
    return p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
}

}  // namespace luma::app::pc
