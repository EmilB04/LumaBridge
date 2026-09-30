// Sony DualSense (PS5 controller) lightbar, by USB or Bluetooth. The output report layout
// below is the one OpenRGB's Sony DualSense driver uses (GPL-2.0-or-later,
// Controllers/SonyGamepadController/SonyDualSenseController), reimplemented here from its
// documented byte offsets rather than copied, since LumaBridge has no DualSense of its own to
// capture from. USB and Bluetooth lighting are confirmed on real hardware.
//
// Report ID 0x02 (USB) / 0x31 (Bluetooth) carries feature-enable flags and the lightbar RGB.
// Bluetooth enables only lighting fields; the established USB packet is kept intact. Bluetooth
// wraps the same payload with a 4-byte CRC-32 (the zlib/PKZIP variant, poly 0xEDB88320, the
// common one, reflected, init/xorout 0xFFFFFFFF) over a leading 0xA2 "direction" byte the
// wire doesn't otherwise carry.
#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>

#include "color.h"

namespace luma::app::dualsense {

constexpr uint16_t kVendor = 0x054C;         // Sony
constexpr uint16_t kProductStandard = 0x0CE6;  // DualSense
constexpr uint16_t kProductEdge = 0x0DF2;      // DualSense Edge

constexpr size_t kUsbReportSize = 48;
constexpr size_t kBtReportSize = 78;

// Windows exposes the largest report in the collection, not just lighting report 0x31.
// Current Bluetooth descriptors can have 547-byte output/feature reports and 78-byte input.
inline bool BluetoothCollection(size_t inputLength, size_t outputLength) {
    return inputLength == 78 && outputLength >= kBtReportSize && outputLength <= 4096;
}
inline bool UsbCollection(size_t inputLength, size_t outputLength) {
    return inputLength == 64 && outputLength >= kUsbReportSize && outputLength <= 4096;
}
template <size_t N>
inline std::vector<uint8_t> HidWriteBuffer(const std::array<uint8_t, N>& report, size_t outputLength) {
    if (outputLength < N || outputLength > 4096) return {};
    std::vector<uint8_t> buffer(outputLength, 0);
    std::copy(report.begin(), report.end(), buffer.begin());
    return buffer;
}

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

// Bluetooth has three header bytes: report ID, four-bit sequence in the high nibble,
// then the fixed 0x10 tag. The 47-byte common payload starts at byte 3, two bytes later
// than USB. See Linux hid-playstation.c and SDL's February 2026 Bluetooth format fix.
inline std::array<uint8_t, kBtReportSize> BtFrame(uint8_t sequence) {
    std::array<uint8_t, kBtReportSize> out{};
    out[0] = 0x31;
    out[1] = static_cast<uint8_t>((sequence & 15) << 4);
    out[2] = 0x10;
    return out;
}

inline void SignBt(std::array<uint8_t, kBtReportSize>* out) {
    std::array<uint8_t, kBtReportSize - 4 + 1> signedBuf{};
    signedBuf[0] = 0xA2;  // HID output direction: signed, but not sent in the report
    for (size_t i = 0; i < kBtReportSize - 4; ++i) signedBuf[i + 1] = (*out)[i];
    const uint32_t crc = Crc32(signedBuf.data(), signedBuf.size());
    for (size_t i = 0; i < 4; ++i) (*out)[kBtReportSize - 4 + i] = static_cast<uint8_t>(crc >> (i * 8));
}

// The controller's Bluetooth startup animation must release the lightbar before colors
// can take effect. Send this once on connection or when reclaiming lighting control.
inline std::array<uint8_t, kBtReportSize> BtResetReport(uint8_t sequence) {
    auto out = BtFrame(sequence);
    out[4] = 0x08;   // common valid_flag1: release the existing LED animation (SDL)
    out[41] = 0x02;  // common valid_flag2: enable lightbar setup
    out[44] = 0x02;  // common lightbar_setup: fade out the startup animation
    SignBt(&out);
    return out;
}

// Only the lightbar and player LEDs are enabled: rumble, audio and triggers keep their
// game's settings. The player indicators stay off, as with the USB lighting mode.
inline std::array<uint8_t, kBtReportSize> BtReport(Rgb color, uint8_t sequence = 0) {
    auto out = BtFrame(sequence);
    out[4] = 0x14;  // common valid_flag1: lightbar color and player indicators
    out[47] = color.r;
    out[48] = color.g;
    out[49] = color.b;
    SignBt(&out);
    return out;
}

}  // namespace luma::app::dualsense
