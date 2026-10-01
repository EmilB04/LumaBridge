// Kraken X3/Z3 RGB commands documented by liquidctl's kraken3 driver. These commands only
// set lighting; they do not set pump/fan duties, firmware, or the LCD. Pure and tested.
#pragma once
#include <array>
#include <cstdint>
#include "color.h"

namespace luma::app::nzxt {
inline uint8_t LightingChannels(uint16_t pid) {
    switch (pid) {
    case 0x2007: case 0x2014: return 0x07;  // X53/X63/X73: ring, logo and external RGB in sync
    case 0x3008: return 0x01;              // Z53/Z63/Z73: external NZXT RGB accessories
    default: return 0;                    // newer LCD models have a separate RGB controller
    }
}

// Fixed color packet, GRB order. A channel mask is supplied by the model, never guessed
// from a vendor name. Repeating fixed colors lets LumaBridge animate its own effects.
inline std::array<uint8_t, 64> FixedLighting(uint8_t channels, Rgb color) {
    std::array<uint8_t, 64> b{};
    b[0] = 0x2A; b[1] = 0x04;
    b[2] = b[3] = channels;
    b[5] = 0x32;  // fixed mode timing
    b[7] = color.g; b[8] = color.r; b[9] = color.b;
    b[56] = 1;    // one color
    b[58] = channels == 0x01 || channels == 0x07 ? 40 : channels == 0x04 ? 1 : 8;
    b[59] = 3;
    return b;
}
}  // namespace luma::app::nzxt
