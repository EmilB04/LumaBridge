// ASUS Aura USB lighting controller protocol (motherboard "AURA LED Controller",
// USB 0B05:1939 on e.g. ROG STRIX B550-F, and its siblings: MainboardProductIds() in
// usb_aura.h), written from the publicly documented behaviour
// of this controller family. Pure packet building / parsing, unit tested; the HID transport
// lives in aura_usb.cpp.
//
// Everything is a 65-byte HID report starting with report id 0xEC:
//   EC 82                      -> EC 02 <firmware name, 16 ASCII bytes>
//   EC B0                      -> EC 30 ?? ?? <60-byte configuration table>
//   EC 35 <ch> 00 00 <mode>    set an *effect channel's* mode; 0xFF = direct (host-driven)
//   EC 36 <mask hi> <mask lo> 00 <R G B>...   effect colors (Static etc.), one per LED slot
//   EC 3F 55                   save the effects and their colors in the controller's flash
//   EC 40 <ch|0x80 on last> <first led> <count> <R G B>...   direct colors, <= 20 LEDs each
//
// Effect channels and direct channels are numbered independently: on a B550-F
// (AULA3-AR42) switching effect channel N to direct and painting direct channel N hit
// different LED groups. Put every effect channel into direct mode first, then address
// direct channels; `aura-usb-test scan` maps which direct channel is which.
//
// Direct mode is never saved: everything LumaBridge shows is undone by Armoury Crate or a
// reboot. The save command (EC 3F 55, as OpenRGB's AsusAuraMainboardController sends it) is
// only used by PowerOnColorRequests(), when the user asks for a power-on color: it writes
// the controller's flash, which shouldn't happen on every color change.
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
constexpr uint8_t kModeStatic = 0x01;
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

// Effect colors for `count` LED slots from `first`. Slots run across the whole controller
// (the board's LEDs first, then one per ARGB header: see EffectTargets()), and the mask
// says which ones this report sets.
inline Report EffectColorRequest(uint8_t first, uint8_t count, Rgb c) {
    Report r = Blank();
    const uint16_t mask = static_cast<uint16_t>(((1u << count) - 1u) << first);
    r[1] = 0x36;
    r[2] = static_cast<uint8_t>(mask >> 8);
    r[3] = static_cast<uint8_t>(mask & 0xFF);
    for (size_t i = first; i < static_cast<size_t>(first) + count; ++i) {
        r[5 + i * 3] = c.r;
        r[6 + i * 3] = c.g;
        r[7 + i * 3] = c.b;
    }
    return r;
}

inline Report SaveRequest() {
    Report r = Blank();
    r[1] = 0x3F;
    r[2] = 0x55;
    return r;
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

// Direct channel of the motherboard's own LEDs (and its 12 V RGB headers). ARGB header i
// is direct channel i. Confirmed on a ROG STRIX B550-F (AULA3-AR42): channel 0 = the ARGB
// header, channel 4 = the board.
constexpr uint8_t kMainboardDirectChannel = 4;
// Effect channels switched to direct mode (their numbering differs from direct channels).
constexpr uint8_t kEffectChannels = 8;
constexpr int kDefaultArgbLeds = 120;

struct UsbChannel {
    std::wstring name;
    uint32_t auraType;  // for the UI: 0x00010000 motherboard, 0x00011000 LED strip
    uint8_t directChannel;
    int leds;
};

inline std::vector<UsbChannel> BuildChannels(const ConfigTable& cfg, int argbLeds = kDefaultArgbLeds) {
    std::vector<UsbChannel> out;
    const int boardLeds = cfg.MainboardLeds() > 0 ? cfg.MainboardLeds() : 5;
    out.push_back(UsbChannel{L"Motherboard LEDs", 0x00010000, kMainboardDirectChannel, boardLeds});
    const int headers = cfg.ArgbHeaders();
    for (int i = 0; i < headers && i < 4; ++i) {
        std::wstring name = headers == 1 ? L"ARGB header (fans, strips)" : L"ARGB header " + std::to_wstring(i + 1);
        out.push_back(UsbChannel{name, 0x00011000, static_cast<uint8_t>(i), argbLeds});
    }
    return out;
}

// An effect channel and the LED slots its effect color goes to (EffectColorRequest).
struct EffectTarget {
    uint8_t effectChannel, firstLed, leds;
    bool operator==(const EffectTarget& o) const {
        return effectChannel == o.effectChannel && firstLed == o.firstLed && leds == o.leds;
    }
};

// The effect channels as OpenRGB's AsusAuraMainboardController numbers them: the board's
// LEDs (if it lists any) are effect channel 0 and the ARGB headers follow, except on
// AULA3-AR32 firmware, where the headers start at 0 and the board comes after them. LED
// slots: the board's LEDs first, then one per header. Slots past the 16-bit mask are left out.
inline std::vector<EffectTarget> EffectTargets(const ConfigTable& cfg, const std::string& firmware) {
    std::vector<EffectTarget> out;
    const int board = cfg.MainboardLeds(), headers = cfg.ArgbHeaders();
    const bool headersFromZero = firmware.rfind("AULA3-AR32", 0) == 0;
    int effect = 0, slot = 0;
    auto add = [&](int channel, int leds) {
        if (slot + leds <= 16) out.push_back({static_cast<uint8_t>(channel), static_cast<uint8_t>(slot), static_cast<uint8_t>(leds)});
        slot += leds;
    };
    if (board > 0) {
        add(headersFromZero ? headers : effect, board);
        if (!headersFromZero) ++effect;
    }
    for (int i = 0; i < headers; ++i) add(effect++, 1);
    return out;
}

// Everything to make the controller show `c`, still, from power-on: every effect channel
// to Static with that color, then the save. Sent once, when the user asks for it.
inline std::vector<Report> PowerOnColorRequests(const ConfigTable& cfg, const std::string& firmware, Rgb c) {
    std::vector<Report> out;
    for (const EffectTarget& t : EffectTargets(cfg, firmware)) {
        out.push_back(SetModeRequest(t.effectChannel, kModeStatic));
        out.push_back(EffectColorRequest(t.firstLed, t.leds, c));
    }
    if (!out.empty()) out.push_back(SaveRequest());
    return out;
}

}  // namespace luma::aurausb
