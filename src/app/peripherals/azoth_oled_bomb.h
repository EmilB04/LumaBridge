#pragma once
#include "azoth_oled_music.h"
#include "azoth_oled_upload.h"
#include "games/cs2_bomb.h"

namespace luma::app::azoth {
// The live bitmap uses the same RAM-only display path as song information.
inline std::vector<uint8_t> RenderBomb(const games::BombCountdown& bomb) {
    std::vector<uint8_t> pixels(208 * 64);
    if (!bomb.active) return pixels;
    auto rect = [&](int x, int y, int w, int h) {
        for (int yy = std::max(0, y); yy < std::min(64, y + h); ++yy)
            for (int xx = std::max(0, x); xx < std::min(208, x + w); ++xx) pixels[yy * 208 + xx] = 255;
    };
    // Five columns per letter, low bit at the top.
    auto glyph = [](char c) -> std::array<uint8_t, 5> {
        switch (c) {
        case 'A': return {126,17,17,17,126}; case 'B': return {127,73,73,73,54};
        case 'C': return {62,65,65,65,34}; case 'D': return {127,65,65,34,28};
        case 'E': return {127,73,73,73,65}; case 'I': return {0,65,127,65,0};
        case 'L': return {127,64,64,64,64}; case 'M': return {127,2,12,2,127};
        case 'N': return {127,4,8,16,127}; case 'O': return {62,65,65,65,62};
        case 'P': return {127,9,9,9,6}; case 'R': return {127,9,25,41,70};
        case 'S': return {38,73,73,73,50}; case 'T': return {1,1,127,1,1};
        case 'Y': return {3,4,120,4,3};
        default: return {};
        }
    };
    auto text = [&](const std::string& str, int y) {
        int x = (208 - static_cast<int>(str.size()) * 6 + 1) / 2;
        for (char c : str) {
            const auto bits = glyph(c);
            for (int col = 0; col < 5; ++col)
                for (int row = 0; row < 7; ++row) if (bits[col] & (1 << row)) rect(x + col, y + row, 1, 1);
            x += 6;
        }
    };
    text(bomb.testing ? "DISPLAY TEST" : "BOMB PLANTED", 2);
    const std::string digits = std::to_string(std::clamp(bomb.seconds, 0, 120));
    // Seven segments, clockwise from the top, with the middle last.
    constexpr std::array<uint8_t, 10> segments{63,6,91,79,102,109,125,7,127,111};
    int x = (208 - static_cast<int>(digits.size()) * 32 + 5) / 2;
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
    text(bomb.estimated ? "ESTIMATED SECONDS" : "SECONDS", 56);
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
