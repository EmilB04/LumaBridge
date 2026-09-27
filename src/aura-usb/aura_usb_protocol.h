// ASUS Aura USB lighting controller protocol (motherboard "AURA LED Controller",
// USB 0B05:1939 on e.g. ROG STRIX B550-F), written from the publicly documented behaviour
// of this controller family. Pure packet building / parsing, unit tested; the HID transport
// lives in aura_usb.cpp.
//
// Everything is a 65-byte HID report starting with report id 0xEC:
//   EC 82                      -> EC 02 <firmware name, 16 ASCII bytes>
//   EC B0                      -> EC 30 ?? ?? <60-byte configuration table>
//   EC 35 <ch> 00 00 <mode>    set a channel's effect; mode 0xFF = direct (host-driven)
//   EC 40 <ch|0x80 on last> <first led> <count> <R G B>...   direct colors, <= 20 LEDs each
//
// Deliberately NOT implemented: the command that saves the current state to the
// controller's flash. Everything LumaBridge does is undone by Armoury Crate or a reboot.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "color.h"

namespace luma::aurausb {

constexpr uint16_t kVendorAsus = 0x0B05;
constexpr uint8_t kReportId = 0xEC;
constexpr size_t kReportSize = 65;
constexpr uint8_t kModeDirect = 0xFF;
constexpr size_t kLedsPerPacket = 20;  // (65 - 5 header bytes) / 3

using Report = std::array<uint8_t, kReportSize>;

inline Report Blank() {
    Report r{};
    r[0] = kReportId;
    return r;
}

inline Report FirmwareRequest() {
    Report r = Blank();
    r[1] = 0x82;
    return r;
}

inline Report ConfigRequest() {
    Report r = Blank();
    r[1] = 0xB0;
    return r;
}

inline Report SetModeRequest(uint8_t channel, uint8_t mode) {
    Report r = Blank();
    r[1] = 0x35;
    r[2] = channel;
    r[5] = mode;
    return r;
}

// Direct colors for `leds` on `channel`, split into packets; the last one carries the
// "apply" flag (0x80) so the controller latches the whole frame at once.
inline std::vector<Report> DirectColorRequests(uint8_t channel, const std::vector<Rgb>& leds) {
    std::vector<Report> out;
    for (size_t first = 0; first < leds.size(); first += kLedsPerPacket) {
        const size_t count = std::min(kLedsPerPacket, leds.size() - first);
        Report r = Blank();
        r[1] = 0x40;
        r[2] = static_cast<uint8_t>(channel | (first + count >= leds.size() ? 0x80 : 0x00));
        r[3] = static_cast<uint8_t>(first);
        r[4] = static_cast<uint8_t>(count);
        for (size_t i = 0; i < count; ++i) {
            r[5 + i * 3] = leds[first + i].r;
            r[6 + i * 3] = leds[first + i].g;
            r[7 + i * 3] = leds[first + i].b;
        }
        out.push_back(r);
    }
    return out;
}

inline bool ParseFirmware(const Report& r, std::string* name) {
    if (r[0] != kReportId || r[1] != 0x02) return false;
    name->clear();
    for (size_t i = 2; i < 18 && r[i] >= 0x20 && r[i] < 0x7F; ++i) *name += static_cast<char>(r[i]);
    return !name->empty();
}

struct ConfigTable {
    std::array<uint8_t, 60> raw{};

    // Offsets as documented for this controller family; `aura-usb-test info` prints the raw
    // table so they can be checked against what a given board reports.
    int ArgbHeaders() const { return raw[0x02]; }
    int MainboardLeds() const { return raw[0x1B]; }
    int RgbHeaders() const { return raw[0x1D]; }
};

inline bool ParseConfig(const Report& r, ConfigTable* cfg) {
    if (r[0] != kReportId || r[1] != 0x30) return false;
    for (size_t i = 0; i < cfg->raw.size(); ++i) cfg->raw[i] = r[4 + i];
    return true;
}

}  // namespace luma::aurausb
