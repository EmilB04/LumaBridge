// Sony DualSense (PS5 controller) lightbar, by USB or Bluetooth. The output report layout
// below is the one OpenRGB's Sony DualSense driver uses (GPL-2.0-or-later,
// Controllers/SonyGamepadController/SonyDualSenseController), reimplemented here from its
// documented byte offsets rather than copied, since LumaBridge has no DualSense of its own to
// capture from. Untested on real hardware; say so plainly until it's confirmed.
//
// Report ID 0x02 (USB) / 0x31 (Bluetooth) is a "set state" output report: two feature-enable
// flag bytes gate which fields the controller actually applies, then (among things LumaBridge
// doesn't touch: rumble, trigger effects, mic LED) a lightbar brightness/fade byte, a bitmask
// for the five player-indicator LEDs (left unlit here) and the lightbar's own RGB. Bluetooth
// wraps the same payload with a 4-byte CRC-32 (the zlib/PKZIP variant, poly 0xEDB88320, the
// common one, reflected, init/xorout 0xFFFFFFFF) over a leading 0xA2 "direction" byte the
// wire doesn't otherwise carry.
#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

#include "color.h"

namespace luma::app::dualsense {

constexpr uint16_t kVendor = 0x054C;         // Sony
constexpr uint16_t kProductStandard = 0x0CE6;  // DualSense
constexpr uint16_t kProductEdge = 0x0DF2;      // DualSense Edge

constexpr size_t kUsbReportSize = 48;
constexpr size_t kBtReportSize = 78;

constexpr uint8_t kDirectMode = 0x01;  // SetupZones' "Direct" mode value: take over the lightbar

// A full USB output report (48 bytes, report ID included): direct mode, full brightness,
// no player LEDs, the given lightbar color.
inline std::array<uint8_t, kUsbReportSize> UsbReport(Rgb color) {
    std::array<uint8_t, kUsbReportSize> r{};
    r[0] = 0x02;
    r[1] = 0x0F;
    r[2] = 0x55;
    r[9] = kDirectMode;
    r[39] = 0xFF;  // must be nonzero for the brightness byte below to take effect
    r[43] = 0x00;  // brightness: 0 = full, 2 = dimmest of the controller's three steps
    r[44] = 0x20;  // lightbar-color bit set, all five player-LED bits clear
    r[45] = color.r;
    r[46] = color.g;
    r[47] = color.b;
    return r;
}

// CRC-32 (reflected, poly 0xEDB88320, init/xorout 0xFFFFFFFF - the zlib/PKZIP/Ethernet one).
inline uint32_t Crc32(const uint8_t* data, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
    return ~crc;
}

// A full Bluetooth output report (78 bytes), CRC included. The CRC covers the leading 0xA2
// "direction" byte (the report the controller actually reads starts at byte 1: Windows'
// HID write doesn't send the 0xA2 itself, only what it signs).
inline std::array<uint8_t, kBtReportSize> BtReport(Rgb color) {
    std::array<uint8_t, kBtReportSize + 1> signedBuf{};  // [0] is the 0xA2, not sent
    signedBuf[0] = 0xA2;
    signedBuf[1] = 0x31;
    signedBuf[2] = 0x02;
    signedBuf[3] = 0x0F;
    signedBuf[4] = 0x55;
    signedBuf[11] = kDirectMode;
    signedBuf[41] = 0xFF;
    signedBuf[44] = 0x02;  // bypasses the default blue that shows before any report arrives
    signedBuf[45] = 0x00;  // brightness: 0 = full
    signedBuf[46] = 0x20;
    signedBuf[47] = color.r;
    signedBuf[48] = color.g;
    signedBuf[49] = color.b;

    std::array<uint8_t, kBtReportSize> out{};
    for (size_t i = 0; i < kBtReportSize - 4; ++i) out[i] = signedBuf[i + 1];
    const uint32_t crc = Crc32(signedBuf.data(), kBtReportSize - 4 + 1);  // +1: the leading 0xA2
    out[kBtReportSize - 4] = static_cast<uint8_t>(crc);
    out[kBtReportSize - 3] = static_cast<uint8_t>(crc >> 8);
    out[kBtReportSize - 2] = static_cast<uint8_t>(crc >> 16);
    out[kBtReportSize - 1] = static_cast<uint8_t>(crc >> 24);
    return out;
}

}  // namespace luma::app::dualsense
