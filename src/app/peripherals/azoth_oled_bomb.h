#pragma once
#include <cmath>
#include "azoth_oled_music.h"
#include "azoth_oled_upload.h"
#include "games/cs2_bomb.h"

namespace luma::app::azoth {
// 18 x 16 monochrome mask from the supplied icon in res/cs2-defuse-kit.png.
// Each row starts at its least significant bit.
inline constexpr std::array<uint32_t, 16> kBombKitIcon{
    0x00004, 0x00018, 0x00039, 0x0007E, 0x0FFFC, 0x1FFF0, 0x38160, 0x000C0,
    0x000C0, 0x000C0, 0x000C0, 0x001C0, 0x00180, 0x00300, 0x00700, 0x00C00
};

// The live bitmap uses the same RAM-only display path as song information.
inline std::vector<uint8_t> RenderBomb(const games::BombCountdown& bomb) {
    std::vector<uint8_t> pixels(208 * 64);
    if (!bomb.active) return pixels;
    auto rect = [&](int x, int y, int w, int h, uint8_t value = 255) {
        for (int yy = std::max(0, y); yy < std::min(64, y + h); ++yy)
            for (int xx = std::max(0, x); xx < std::min(208, x + w); ++xx) pixels[yy * 208 + xx] = value;
    };
    auto line = [&](int x, int y, int xx, int yy, uint8_t value = 255) {
        const int dx = std::abs(xx - x), sx = x < xx ? 1 : -1;
        const int dy = -std::abs(yy - y), sy = y < yy ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            rect(x, y, 1, 1, value);
            if (x == xx && y == yy) break;
            const int twice = 2 * error;
            if (twice >= dy) { error += dy; x += sx; }
            if (twice <= dx) { error += dx; y += sy; }
        }
    };
    // Five columns per letter, low bit at the top.
    auto glyph = [](char c) -> std::array<uint8_t, 5> {
        switch (c) {
        case 'A': return {126,17,17,17,126}; case 'B': return {127,73,73,73,54};
        case 'C': return {62,65,65,65,34}; case 'D': return {127,65,65,34,28};
        case 'E': return {127,73,73,73,65}; case 'I': return {0,65,127,65,0};
        case 'K': return {127,8,20,34,65};
        case 'L': return {127,64,64,64,64}; case 'M': return {127,2,12,2,127};
        case 'N': return {127,4,8,16,127}; case 'O': return {62,65,65,65,62};
        case 'P': return {127,9,9,9,6}; case 'R': return {127,9,25,41,70};
        case 'S': return {38,73,73,73,50}; case 'T': return {1,1,127,1,1};
        case 'Y': return {3,4,120,4,3};
        case 'X': return {99,20,8,20,99};
        case '0': return {62,81,73,69,62}; case '1': return {0,66,127,64,0};
        case '5': return {39,69,69,69,57};
        default: return {};
        }
    };
    auto text = [&](const std::string& str, int x, int y, int scale = 1) {
        for (char c : str) {
            const auto bits = glyph(c);
            for (int col = 0; col < 5; ++col)
                for (int row = 0; row < 7; ++row) if (bits[col] & (1 << row))
                    rect(x + col * scale, y + row * scale, scale, scale);
            x += 6 * scale;
        }
    };
    auto centered = [&](const std::string& str, int y, int width) {
        text(str, (width - static_cast<int>(str.size()) * 6 + 1) / 2, y);
    };
    if (bomb.exploded) {
        const int frame = std::clamp(bomb.explosionFrame, 0, 3);
        const int radius = 8 + frame * 13, inner = std::max(0, radius - 5);
        for (int y = 10; y < 51; ++y)
            for (int x = 0; x < 208; ++x) {
                const int d = (x - 104) * (x - 104) + (y - 30) * (y - 30);
                if (d <= radius * radius && d >= inner * inner) rect(x, y, 1, 1);
            }
        for (int ray = 0; ray < 16; ++ray) {
            const double angle = ray * 6.283185307179586 / 16 + frame * 0.13;
            const int start = radius / 2, end = radius + 8 + ray % 3 * 4;
            line(104 + static_cast<int>(std::cos(angle) * start), 30 + static_cast<int>(std::sin(angle) * start),
                 104 + static_cast<int>(std::cos(angle) * end), 30 + static_cast<int>(std::sin(angle) * end));
            if (frame >= 2)
                rect(104 + static_cast<int>(std::cos(angle) * (end + 8)),
                     30 + static_cast<int>(std::sin(angle) * (end + 8)), 2, 2);
        }
        rect(0, 0, 208, 10, 0);
        rect(0, 51, 208, 13, 0);
        centered("BOMB EXPLODED", 2, 208);
        centered("BOOM", 55, 208);
        return pixels;
    }
    constexpr int countdownWidth = 124;
    centered(bomb.testing ? "DISPLAY TEST" : "BOMB PLANTED", 2, countdownWidth);
    const std::string digits = std::to_string(std::clamp(bomb.seconds, 0, 120));
    // Seven segments, clockwise from the top, with the middle last.
    constexpr std::array<uint8_t, 10> segments{63,6,91,79,102,109,125,7,127,111};
    int x = (countdownWidth - static_cast<int>(digits.size()) * 32 + 5) / 2;
    for (char c : digits) {
        const int mask = segments[c - '0'], y = 14;
        if (mask & 1) rect(x + 4, y, 19, 4);
        if (mask & 2) rect(x + 23, y + 4, 4, 13);
        if (mask & 4) rect(x + 23, y + 21, 4, 13);
        if (mask & 8) rect(x + 4, y + 34, 19, 4);
        if (mask & 16) rect(x, y + 21, 4, 13);
        if (mask & 32) rect(x, y + 4, 4, 13);
        if (mask & 64) rect(x + 4, y + 17, 19, 4);
        x += 32;
    }
    centered(bomb.estimated ? "EST SECONDS" : "SECONDS", 56, countdownWidth);
    rect(124, 3, 1, 58);
    auto kit = [&](int y, bool crossedOut) {
        for (int row = 0; row < static_cast<int>(kBombKitIcon.size()); ++row)
            for (int col = 0; col < 18; ++col)
                if (kBombKitIcon[row] & (1u << col)) rect(128 + col, y + row, 1, 1);
        if (crossedOut) {
            // A dark edge keeps the slash distinct where it crosses the cutter handles.
            line(126, y + 16, 146, y - 1, 0);
            line(128, y + 16, 148, y - 1, 0);
            line(127, y + 16, 147, y - 1);
        }
    };
    constexpr int panelTop = (64 - 53) / 2;
    kit(panelTop + 2, false);
    text("KIT", 150, panelTop);
    text("5S", 164, panelTop + 11, 2);
    kit(panelTop + 30, true);
    text("NO KIT", 148, panelTop + 28);
    text("10S", 155, panelTop + 39, 2);
    return pixels;
}

// Keep current cannot reconstruct a vendor's live mode. Restore a known animation,
// otherwise show the clock. Never re-upload the custom GIF to restore it.
inline std::vector<Report> BombRestoreCommands(OledSettings s, Link link, OledTime time,
                                               int customEffect, int preset) {
    if (!s.direct || !s.enabled) return {};
    if (s.content == OledContent::Keep) {
        if (customEffect >= 0) return {OledUploadShow()};
        if (preset >= 0 && preset < 6) return {OledCommand(link, 0x61, static_cast<uint8_t>(preset))};
        return {OledClock(link, time, s.clock12Hour)};
    }
    if (s.content == OledContent::Animation && s.animationSource == OledAnimationSource::LumaBridge &&
        customEffect >= 0) return {OledUploadShow()};
    if (s.content == OledContent::Animation && s.animationSource == OledAnimationSource::LumaBridge)
        return {OledClock(link, time, s.clock12Hour)};
    return {};
}
} // namespace luma::app::azoth
