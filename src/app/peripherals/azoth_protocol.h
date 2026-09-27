// ASUS ROG Azoth (USB 0B05:1A83, wired) lighting, as captured from Armoury Crate:
//
//   51 2C 00 00 FF <bright> 00 FF FF <R> <G> <B>    static color (brightness 0x00-0x64)
//   50 55                                           save to the keyboard's flash
//
// on the vendor interface (MI_01, usage page 0xFF00), 64-byte output reports with report
// ID 0. LumaBridge only ever sends the color command, never the save: what it shows lasts
// until the keyboard restarts, and the lighting Armoury Crate saved stays in the keyboard.
// Pure packet building, tested.
#pragma once

#include <array>
#include <cstdint>

#include "color.h"

namespace luma::app::azoth {

constexpr uint16_t kVendor = 0x0B05;
constexpr uint16_t kProductWired = 0x1A83;
constexpr uint16_t kUsagePage = 0xFF00;
constexpr size_t kReportSize = 65;  // report ID + 64 data bytes

using Report = std::array<uint8_t, kReportSize>;

// Static color at full keyboard brightness (LumaBridge's own brightness is applied to the
// color itself, so dimming is smooth and the same as on the fans).
inline Report StaticColor(Rgb c) {
    Report r{};
    const uint8_t cmd[] = {0x51, 0x2C, 0x00, 0x00, 0xFF, 0x64, 0x00, 0xFF, 0xFF, c.r, c.g, c.b};
    for (size_t i = 0; i < sizeof cmd; ++i) r[1 + i] = cmd[i];  // r[0] = report ID 0
    return r;
}

// True for commands LumaBridge must never send (writing the keyboard's flash).
inline bool IsSave(const Report& r) { return r[1] == 0x50; }

}  // namespace luma::app::azoth
