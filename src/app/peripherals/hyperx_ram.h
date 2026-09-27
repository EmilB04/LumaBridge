// HyperX / Kingston FURY RGB DDR4 lighting over the SMBus (the protocol OpenRGB documents
// in Controllers/HyperXDRAMController):
//
//   one controller at SMBus address 0x27 for all sticks, 5 LEDs per stick
//   E1 = 01                       start an update
//   E5 = 21                       direct mode (each LED its own color)
//   base + 3*led + 0/1/2 = R/G/B  per LED; base 0x11 / 0x41 / 0x71 / 0xA1 for SPD slots 0-3
//   base + 0x10 + 3*led = 0..100  per-LED brightness
//   E1 = 02, E1 = 03              apply
//   E3 = 05, D1/D2 = timer        the sticks' own rainbow (used when LumaBridge lets go)
//
// The sticks' own configuration chips (SPD, 0x50-0x57) and every other address are never
// written: the only address is kController, and every write goes through IsAllowed(), which
// admits only the registers above. Pure, tested.
// The SMBus writes happen in the elevated hardware helper (src/helper).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "color.h"

namespace luma::app::ram {

constexpr uint8_t kController = 0x27;   // SMBus address of the RGB controller
constexpr uint8_t kSpdFirst = 0x50;     // SPD EEPROM of slot 0 (read only, to find the sticks)
constexpr int kSlots = 4;
constexpr int kLedsPerStick = 5;
constexpr int kMaxLeds = kSlots * kLedsPerStick;

constexpr uint8_t kRegApply = 0xE1;
constexpr uint8_t kRegDirect = 0xE5;
constexpr uint8_t kDirectMode = 0x21;
constexpr uint8_t kFullBrightness = 0x64;  // 100
constexpr uint8_t kRegModeRandom = 0xE3, kRainbowMode = 0x05;
constexpr uint8_t kRegTimerHigh = 0xD1, kRegTimerLow = 0xD2;
constexpr uint16_t kRainbowTimer = 0x07D0;  // the sticks' normal rainbow speed

inline uint8_t SlotBase(int slot) {
    static const uint8_t kBase[kSlots] = {0x11, 0x41, 0x71, 0xA1};
    return kBase[slot];
}

struct Write {
    uint8_t reg, value;
};

// Whether LumaBridge may ever write `reg` of the controller.
inline bool IsAllowed(uint8_t reg) {
    if (reg == kRegApply || reg == kRegDirect || reg == kRegModeRandom || reg == kRegTimerHigh || reg == kRegTimerLow)
        return true;
    for (int s = 0; s < kSlots; ++s)
        for (int led = 0; led < kLedsPerStick; ++led) {
            const int color = SlotBase(s) + 3 * led, bright = SlotBase(s) + 0x10 + 3 * led;
            if (reg >= color && reg <= color + 2) return true;
            if (reg == bright) return true;
        }
    return false;
}

// Register writes for one frame. `sticks`: bit i = a stick in SPD slot i. `colors`: LED `l`
// of slot `s` at colors[s * kLedsPerStick + l].
inline std::vector<Write> Frame(uint8_t sticks, const std::array<Rgb, kMaxLeds>& colors) {
    std::vector<Write> w;
    w.push_back({kRegApply, 0x01});
    for (int s = 0; s < kSlots; ++s) {
        if (!(sticks & (1 << s))) continue;
        w.push_back({kRegDirect, kDirectMode});
        const uint8_t base = SlotBase(s);
        for (int led = 0; led < kLedsPerStick; ++led) {
            const Rgb c = colors[static_cast<size_t>(s * kLedsPerStick + led)];
            const uint8_t at = static_cast<uint8_t>(base + 3 * led);
            w.push_back({at, c.r});
            w.push_back({static_cast<uint8_t>(at + 1), c.g});
            w.push_back({static_cast<uint8_t>(at + 2), c.b});
            w.push_back({static_cast<uint8_t>(at + 0x10), kFullBrightness});
        }
    }
    w.push_back({kRegApply, 0x02});
    w.push_back({kRegApply, 0x03});
    return w;
}

// Register writes that hand the sticks back to their own rainbow effect.
inline std::vector<Write> OwnRainbow() {
    return {{kRegApply, 0x01},
            {kRegModeRandom, kRainbowMode},
            {kRegTimerHigh, static_cast<uint8_t>(kRainbowTimer >> 8)},
            {kRegTimerLow, static_cast<uint8_t>(kRainbowTimer & 0xFF)},
            {kRegApply, 0x02},
            {kRegApply, 0x03}};
}

}  // namespace luma::app::ram
