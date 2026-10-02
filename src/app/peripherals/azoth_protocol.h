// ASUS ROG Azoth lighting, as captured from Armoury Crate:
//
//   51 2C <mode> <n> <speed> <bright> <flag> <data...>   an effect (brightness 0x00-0x64)
//   51 2C <mode> <n-1> <data...>                          ... its continuation packets
//   50 55                                                  save to the keyboard's flash
//
// <n> counts the packets still to come, down to 0. The effect's data runs on from byte 7
// of the first packet (11 bytes) into byte 4 of each continuation (16 bytes each). The
// modes are the ones Armoury Crate offers, in its order (docs/peripherals.md).
//
// Wired (USB 0B05:1A83): the vendor interface (MI_01, usage page 0xFF00), 64-byte output
// reports with report ID 0. Wireless (ROG Omni receiver 0B05:1ACE): the same commands on
// the receiver's MI_02 vendor collection (usage page 0xFF00), report ID 2, 63 data bytes;
// the receiver passes them on to the keyboard. LumaBridge never sends the save:
// what it shows lasts until the keyboard restarts, and the
// lighting Armoury Crate saved stays in the keyboard. Pure packet building, tested.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "color.h"

namespace luma::app::azoth {

constexpr uint16_t kVendor = 0x0B05;
constexpr uint16_t kProductWired = 0x1A83;
constexpr uint16_t kProductReceiver = 0x1ACE;  // ROG Omni receiver (2.4 GHz)
constexpr uint16_t kUsagePage = 0xFF00;
constexpr size_t kReportSize = 65;  // largest report: ID + 64 data bytes (wired)

enum class Link { Wired, Wireless };

using Report = std::array<uint8_t, kReportSize>;

// Bytes to write for a link (report ID included) - HID writes must be exactly this long.
constexpr size_t ReportSize(Link l) { return l == Link::Wired ? 65 : 64; }
constexpr uint16_t Product(Link l) { return l == Link::Wired ? kProductWired : kProductReceiver; }

enum class Mode : uint8_t {
    Static = 0x00,
    Breathing = 0x01,
    ColorCycle = 0x02,
    Reactive = 0x03,
    Wave = 0x04,
    Ripple = 0x05,
    StarryNight = 0x06,
    Quicksand = 0x07,
    Current = 0x08,
    RainDrop = 0x09,
};

constexpr uint8_t kFullBrightness = 0x64;
constexpr uint8_t kStaticSpeed = 0xFF;  // what Armoury Crate sends for the effects that don't move
// <flag> values seen: 00 one color, 01 random colors, 10 breathing between two colors.
constexpr uint8_t kOneColor = 0x00, kRandomColors = 0x01, kTwoColors = 0x10;
constexpr size_t kFirstData = 11, kMoreData = 16;

struct Effect {
    Mode mode = Mode::Static;
    uint8_t speed = kStaticSpeed;  // Armoury Crate sent 0x07-0x64 (which way is faster: not known yet)
    uint8_t brightness = kFullBrightness;
    uint8_t flag = kOneColor;
    std::vector<uint8_t> data;
};

// The packets for an effect, in the order to send them.
inline std::vector<Report> Packets(const Effect& e, Link link = Link::Wired) {
    const size_t rest = e.data.size() > kFirstData ? e.data.size() - kFirstData : 0;
    const size_t count = 1 + (rest + kMoreData - 1) / kMoreData;
    std::vector<Report> out(count);
    size_t at = 0;
    for (size_t p = 0; p < count; ++p) {
        Report& r = out[p];
        r[0] = link == Link::Wired ? 0x00 : 0x02;  // report ID
        r[1] = 0x51;
        r[2] = 0x2C;
        r[3] = static_cast<uint8_t>(e.mode);
        r[4] = static_cast<uint8_t>(count - 1 - p);
        size_t pos = 5, room = kMoreData;
        if (p == 0) {
            r[5] = e.speed;
            r[6] = e.brightness;
            r[7] = e.flag;
            pos = 8;
            room = kFirstData;
        }
        for (size_t i = 0; i < room && at < e.data.size(); ++i) r[pos + i] = e.data[at++];
    }
    return out;
}

// Static color at full keyboard brightness (LumaBridge's own brightness is applied to the
// color itself, so dimming is smooth and the same as on the fans).
inline Report StaticColor(Rgb c, Link link = Link::Wired) {
    return Packets(Effect{Mode::Static, kStaticSpeed, kFullBrightness, kOneColor, {0xFF, 0xFF, c.r, c.g, c.b}}, link)[0];
}

// Breathing: one color, or fading between two.
inline Effect Breathing(Rgb a, uint8_t speed, uint8_t brightness) {
    return Effect{Mode::Breathing, speed, brightness, kOneColor, {0xFF, 0xFF, a.r, a.g, a.b}};
}
inline Effect Breathing(Rgb a, Rgb b, uint8_t speed, uint8_t brightness) {
    return Effect{Mode::Breathing, speed, brightness, kTwoColors, {0xFF, 0xFF, a.r, a.g, a.b, b.r, b.g, b.b}};
}

inline Effect ColorCycle(uint8_t speed, uint8_t brightness) {
    return Effect{Mode::ColorCycle, speed, brightness, kOneColor, {0xFF, 0xFF}};
}

// Armoury Crate's rainbow: 7 stops of <position 0-100> <R> <G> <B>, purple to red.
inline std::vector<uint8_t> RainbowStops() {
    return {0x0E, 0xF5, 0x00, 0xFF, 0x1D, 0x00, 0x06, 0xFF, 0x2B, 0x00, 0xFA, 0xFF, 0x39, 0x01,
            0xFF, 0x00, 0x48, 0xFF, 0xF6, 0x00, 0x56, 0xFF, 0x78, 0x07, 0x64, 0xFF, 0x00, 0x0D};
}

// Wave and ripple over a gradient (Armoury Crate's rainbow). Byte 7 was 00 for wave and FF
// for ripple, byte 8 02 for both (direction / width: not known yet); then the stop count.
inline Effect RainbowWave(uint8_t speed, uint8_t brightness) {
    Effect e{Mode::Wave, speed, brightness, kOneColor, {0x00, 0x02, 0x07}};
    const auto stops = RainbowStops();
    e.data.insert(e.data.end(), stops.begin(), stops.end());
    return e;
}
inline Effect RainbowRipple(uint8_t speed, uint8_t brightness) {
    Effect e{Mode::Ripple, speed, brightness, kRandomColors, {0xFF, 0x02, 0x07}};
    const auto stops = RainbowStops();
    e.data.insert(e.data.end(), stops.begin(), stops.end());
    return e;
}

// Quicksand: six colors (Armoury Crate's are yellow, green, cyan, blue, purple, red).
inline Effect Quicksand(const Rgb (&c)[6], uint8_t speed, uint8_t brightness) {
    Effect e{Mode::Quicksand, speed, brightness, kRandomColors, {0x02, 0xFF}};
    for (const Rgb& x : c) e.data.insert(e.data.end(), {x.r, x.g, x.b});
    return e;
}

// Reactive, starry night, current and rain drop: one color, or random colors.
inline Effect Simple(Mode m, Rgb c, bool random, uint8_t speed, uint8_t brightness) {
    return Effect{m, speed, brightness, random ? kRandomColors : kOneColor, {0xFF, 0xFF, c.r, c.g, c.b}};
}

// Per-key colors (not in Armoury Crate's captures; the command ASUS ROG keyboards take,
// tested on a wired Azoth): C0 81 <n> 00, then n x <LED number> <R> <G> <B>. The LED numbers
// are in azoth_layout.h. As many keys as fit the link's report: 15 by cable (65 bytes), 14
// through the Omni receiver (64 bytes: a 15th key would lose its blue).
constexpr size_t KeysPerReport(Link l) { return (ReportSize(l) - 5) / 4; }
struct KeyColor {
    uint8_t led;
    Rgb color;
};
inline std::vector<Report> KeyColors(const std::vector<KeyColor>& keys, Link link = Link::Wired) {
    std::vector<Report> out;
    const size_t per = KeysPerReport(link);
    for (size_t first = 0; first < keys.size(); first += per) {
        const size_t n = std::min(per, keys.size() - first);
        Report r{};
        r[0] = link == Link::Wired ? 0x00 : 0x02;
        r[1] = 0xC0;
        r[2] = 0x81;
        r[3] = static_cast<uint8_t>(n);
        for (size_t k = 0; k < n; ++k) {
            const KeyColor& kc = keys[first + k];
            r[5 + k * 4] = kc.led;
            r[6 + k * 4] = kc.color.r;
            r[7 + k * 4] = kc.color.g;
            r[8 + k * 4] = kc.color.b;
        }
        out.push_back(r);
    }
    return out;
}

// True for commands LumaBridge must never send (writing the keyboard's flash).
inline bool IsSave(const Report& r) { return r[1] == 0x50; }

}  // namespace luma::app::azoth
