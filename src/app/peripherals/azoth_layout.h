// The ROG Azoth's keys: where each sits and which LED lights it (mapped with azoth-probe on
// an ISO / Nordic Azoth). LED number = column * 8 + row: row 0 is the F-row, row 5 the
// bottom row; columns run left to right, 16 units across. Used both to light the keys and to
// draw the keyboard in the preview. Pure, tested.
#pragma once

#include <initializer_list>
#include <utility>
#include <vector>

#include "color.h"
#include "effects.h"

namespace luma::app::azoth {

struct Key {
    int led;
    float x, y, w, h;  // units: a key is 1 x 1; rows at y = 0 (F-row) .. 5
};

// ISO 75 %: the tall Enter, the key left of Z, and the one left of Enter.
inline const std::vector<Key>& IsoKeys() {
    static const std::vector<Key> kKeys = [] {
        std::vector<Key> k;
        auto row = [&](float y, std::initializer_list<std::pair<int, float>> keys, float x = 0) {
            for (const auto& [led, w] : keys) {
                if (led >= 0) k.push_back({led, x, y, w, 1});
                x += w < 0 ? -w : w;
            }
        };
        // F-row: Esc, then F1-F4, F5-F8, F9-F12 in groups (-0.25: a gap). The knob and screen
        // (no LEDs) fill the rest.
        row(0, {{0, 1}, {-1, -0.25f}, {8, 1}, {16, 1}, {24, 1}, {32, 1}, {-1, -0.25f}, {40, 1}, {48, 1}, {56, 1},
                {64, 1}, {-1, -0.25f}, {72, 1}, {80, 1}, {88, 1}, {96, 1}});
        // Numbers: § 1 ... 0 + \, Backspace, then the right-hand column.
        row(1, {{1, 1}, {9, 1}, {17, 1}, {25, 1}, {33, 1}, {41, 1}, {49, 1}, {57, 1}, {65, 1}, {73, 1}, {81, 1},
                {89, 1}, {97, 1}, {105, 2}, {121, 1}});
        // Tab Q ... P Å ¨, then the tall Enter's top (added below), the right column.
        row(2, {{2, 1.5f}, {10, 1}, {18, 1}, {26, 1}, {34, 1}, {42, 1}, {50, 1}, {58, 1}, {66, 1}, {74, 1}, {82, 1},
                {90, 1}, {98, 1}, {-1, -1.5f}, {122, 1}});
        // Caps A ... L Ø Æ ', the Enter's foot, the right column.
        row(3, {{3, 1.75f}, {11, 1}, {19, 1}, {27, 1}, {35, 1}, {43, 1}, {51, 1}, {59, 1}, {67, 1}, {75, 1}, {83, 1},
                {91, 1}, {99, 1}, {-1, -1.25f}, {123, 1}});
        // Shift < Z ... - Shift, Up, the right column.
        row(4, {{4, 1.25f}, {12, 1}, {20, 1}, {28, 1}, {36, 1}, {44, 1}, {52, 1}, {60, 1}, {68, 1}, {76, 1}, {84, 1},
                {92, 1}, {100, 1.75f}, {116, 1}, {124, 1}});
        // Ctrl Win Alt Space AltGr Fn Ctrl, Left Down Right.
        row(5, {{5, 1.25f}, {13, 1.25f}, {21, 1.25f}, {53, 6.25f}, {85, 1}, {93, 1}, {101, 1}, {109, 1}, {117, 1},
                {125, 1}});
        k.push_back({107, 13.75f, 2, 1.5f, 2});  // the tall Enter, over rows 2 and 3
        return k;
    }();
    return kKeys;
}

constexpr int kColumns = 32;  // effects run across the keyboard in this many steps

// Where a key sits for an effect: 0..kColumns-1 by its center, left to right.
inline int EffectColumn(const Key& k) {
    const int c = static_cast<int>((k.x + k.w / 2) / 16.f * kColumns);
    return c < 0 ? 0 : c >= kColumns ? kColumns - 1 : c;
}

// Every key's color for an effect at time `t` (same order as IsoKeys()).
inline std::vector<Rgb> RenderKeys(const fx::Params& p, double t, double brightness = 1.0) {
    std::vector<Rgb> out;
    for (const Key& k : IsoKeys()) out.push_back(Scale(fx::Render(p, t, EffectColumn(k), kColumns), brightness));
    return out;
}

}  // namespace luma::app::azoth
