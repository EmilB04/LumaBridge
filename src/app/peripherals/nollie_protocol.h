// Nollie ARGB hubs (1, 8, 16 and 32 channels, the 28 series, Prism8), over USB HID. The
// protocol and the device table follow OpenRGB's Nollie driver (Controllers/NollieController);
// only colors are sent. (What the hub keeps on its own is asked for in nollie_onboard.h.)
// Pure and tested.
//
// Two families speak two dialects:
//   - 16 / 32 channel hubs ("Hs"): one 1025-byte report per channel carrying all its LEDs,
//     colors in G, R, B order. Channels go out in hardware order; the channels 15 and 31 are
//     flags that end a bank of channels and always go out, even with no LEDs on them.
//   - every other hub ("Fs"): reports of 64 bytes with at most 21 LEDs each, then one update
//     report that shows the frame.
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <string>
#include <vector>

#include "color.h"

namespace luma::app::nollie {

enum class Family { Hs, Fs };

// How a zone number (the channel as the user counts it) maps to the hardware channel.
enum class Wiring { Straight, Hub32, Hub16Early, Hub16 };

struct Model {
    uint16_t vid, pid;
    const char* name;
    int channels;     // zones the hub has
    int maxLeds;      // LEDs on one channel
    Family family;
    Wiring wiring;
    int interfaceNumber;  // USB interface carrying the lighting reports; -1: any that fits
};

constexpr uint16_t kVidNollie = 0x16D2, kVidNollieOs2 = 0x16D5, kVidNollieHs = 0x3061;

inline const std::vector<Model>& Models() {
    using F = Family;
    using W = Wiring;
    static const std::vector<Model> k{
        {0x3061, 0x4714, "Nollie 32CH", 32, 256, F::Hs, W::Hub32, -1},
        {0x3061, 0x4716, "Nollie 16CH", 16, 256, F::Hs, W::Hub16Early, -1},
        {0x16D2, 0x1F01, "Nollie 8CH", 8, 126, F::Fs, W::Straight, -1},
        {0x16D2, 0x1F11, "Nollie 1CH", 1, 630, F::Fs, W::Straight, -1},
        {0x16D2, 0x1616, "Nollie 28 12", 1, 42, F::Fs, W::Straight, -1},
        {0x16D2, 0x1617, "Nollie 28 L1", 8, 525, F::Fs, W::Straight, -1},
        {0x16D2, 0x1618, "Nollie 28 L2", 8, 525, F::Fs, W::Straight, -1},
        // The same hubs with Nollie's OS2 firmware.
        {0x16D5, 0x4714, "Nollie 32CH (OS2)", 32, 256, F::Hs, W::Hub32, -1},
        {0x16D5, 0x4716, "Nollie 16CH (OS2)", 16, 256, F::Hs, W::Hub16, -1},
        {0x16D5, 0x1F01, "Nollie 8CH (OS2)", 8, 126, F::Fs, W::Straight, -1},
        {0x16D5, 0x1F11, "Nollie 1CH (OS2)", 1, 630, F::Fs, W::Straight, -1},
        // OS2 composite devices: the lighting reports sit on one USB interface.
        {0x16D5, 0x2A32, "Nollie 32CH (OS2)", 32, 256, F::Hs, W::Hub32, 0},
        {0x16D5, 0x2A16, "Nollie 16CH (OS2)", 16, 256, F::Hs, W::Hub16, 0},
        {0x16D5, 0x2A08, "Nollie 8CH (OS2)", 8, 126, F::Fs, W::Straight, 2},
        {0x16D5, 0x2C08, "Prism8", 8, 126, F::Fs, W::Straight, 2},
        {0x16D5, 0x2A01, "Nollie 1CH (OS2)", 1, 630, F::Fs, W::Straight, 2},
    };
    return k;
}

inline const Model* Find(uint16_t vid, uint16_t pid) {
    for (const Model& m : Models())
        if (m.vid == vid && m.pid == pid) return &m;
    return nullptr;
}

constexpr int kHsReport = 1025, kFsReport = 65;
constexpr int kLedsPerFsPacket = 21;
constexpr int kFlagChannel1 = 15, kFlagChannel2 = 31;
constexpr int kFlagDelayMs = 8;  // the hub needs a moment after a flag channel

// The hardware channel of zone `zone` (0-based).
inline int HardwareChannel(const Model& m, int zone) {
    static const int hub32[32] = {5, 4, 3, 2, 1, 0, 15, 14, 26, 27, 28, 29, 30, 31, 8, 9,
                                  19, 18, 17, 16, 7, 6, 25, 24, 23, 22, 21, 20, 13, 12, 11, 10};
    static const int hub16Early[16] = {19, 18, 17, 16, 24, 25, 26, 27, 20, 21, 22, 23, 31, 30, 29, 28};
    static const int hub16[16] = {3, 2, 1, 0, 8, 9, 10, 11, 4, 5, 6, 7, 15, 14, 13, 12};
    if (zone < 0 || zone >= m.channels) return zone;
    switch (m.wiring) {
    case Wiring::Hub32: return hub32[zone];
    case Wiring::Hub16Early: return hub16Early[zone];
    case Wiring::Hub16: return hub16[zone];
    default: return zone;
    }
}

// The label printed next to the hub's headers, as OpenRGB names them.
inline std::string ChannelName(int zone) {
    if (zone > 27) return "Channel EXT " + std::to_string(zone + 1 - 28);
    if (zone > 21) return "Channel GPU " + std::to_string(zone + 1 - 22);
    if (zone > 15) return "Channel ATX " + std::to_string(zone + 1 - 16);
    return "Channel " + std::to_string(zone + 1);
}

struct Packet {
    std::vector<uint8_t> bytes;  // report ID (0) first
    int delayMs = 0;             // wait this long after sending it
};

// Fs hubs of some generations take G, R, B.
inline bool SwapsRedGreen(const Model& m) {
    return m.pid == 0x1F01 || m.pid == 0x1F11 || m.pid == 0x2A08 || m.pid == 0x2A01 || m.pid == 0x2C08;
}

// Reports of Fs hubs carry a packet number made from the channel and the packet's place;
// the spacing is how many packets a channel may need.
inline int PacketInterval(const Model& m) {
    switch (m.pid) {
    case 0x1616: return 2;
    case 0x1F01: case 0x2A08: case 0x2C08: return 6;
    case 0x1F11: case 0x2A01: return 30;
    default: return 25;
    }
}

// All the LEDs of a channel on a 16 / 32 channel hub.
inline Packet HsPacket(int hardwareChannel, const std::vector<Rgb>& leds) {
    Packet p;
    p.bytes.assign(kHsReport, 0);
    const int n = static_cast<int>(std::min<size_t>(leds.size(), 340));  // what a report holds
    p.bytes[1] = static_cast<uint8_t>(hardwareChannel);
    p.bytes[3] = static_cast<uint8_t>(n / 256);
    p.bytes[4] = static_cast<uint8_t>(n % 256);
    if (hardwareChannel == kFlagChannel1) { p.bytes[2] = 1; p.delayMs = kFlagDelayMs; }
    if (hardwareChannel == kFlagChannel2) { p.bytes[2] = 2; p.delayMs = kFlagDelayMs; }
    for (int i = 0; i < n; ++i) {
        p.bytes[5 + i * 3] = leds[i].g;
        p.bytes[6 + i * 3] = leds[i].r;
        p.bytes[7 + i * 3] = leds[i].b;
    }
    return p;
}

// A channel's LEDs on the other hubs: 21 to a report.
inline std::vector<Packet> FsPackets(const Model& m, int channel, const std::vector<Rgb>& leds) {
    std::vector<Packet> out;
    const int interval = PacketInterval(m);
    const bool swap = SwapsRedGreen(m);
    const int n = static_cast<int>(leds.size());
    for (int packet = 0, at = 0; at < n; ++packet, at += kLedsPerFsPacket) {
        Packet p;
        p.bytes.assign(kFsReport, 0);
        p.bytes[1] = static_cast<uint8_t>(packet + channel * interval);
        const int here = std::min(kLedsPerFsPacket, n - at);
        for (int i = 0; i < here; ++i) {
            const Rgb c = leds[at + i];
            p.bytes[2 + i * 3] = swap ? c.g : c.r;
            p.bytes[3 + i * 3] = swap ? c.r : c.g;
            p.bytes[4 + i * 3] = c.b;
        }
        out.push_back(std::move(p));
    }
    return out;
}

// Shows what was sent (Fs hubs).
inline Packet UpdatePacket() {
    Packet p;
    p.bytes.assign(kFsReport, 0);
    p.bytes[1] = 0xFF;
    return p;
}

// One frame: `zones[z]` are the LEDs of zone z (empty: nothing connected). Returns the reports
// in the order to send them. LEDs past what a channel holds are dropped.
inline std::vector<Packet> Frame(const Model& m, const std::vector<std::vector<Rgb>>& zones) {
    std::vector<Packet> out;
    auto leds = [&](int zone) {
        std::vector<Rgb> v = zone < static_cast<int>(zones.size()) ? zones[zone] : std::vector<Rgb>{};
        if (static_cast<int>(v.size()) > m.maxLeds) v.resize(m.maxLeds);
        return v;
    };
    if (m.family == Family::Hs) {
        // Hardware order, with the flag channels always there.
        std::vector<std::pair<int, int>> order;  // hardware channel, zone
        for (int z = 0; z < m.channels; ++z) {
            const int hw = HardwareChannel(m, z);
            if (!leds(z).empty() || hw == kFlagChannel1 || hw == kFlagChannel2) order.emplace_back(hw, z);
        }
        std::sort(order.begin(), order.end());
        for (const auto& [hw, z] : order) out.push_back(HsPacket(hw, leds(z)));
    } else {
        for (int z = 0; z < m.channels; ++z) {
            const auto l = leds(z);
            if (l.empty()) continue;
            for (Packet& p : FsPackets(m, z, l)) out.push_back(std::move(p));
        }
        out.push_back(UpdatePacket());
    }
    return out;
}

// LED counts the hub has to be told about. The 1-channel hub learns its length this way; the
// original 32-channel hub switches a power stage for long chains on its 27th channel.
// `previous`: the counts last sent (empty: none yet). Nothing for hubs with no such setting.
inline std::vector<Packet> ConfigPackets(const Model& m, const std::vector<int>& previous, const std::vector<int>& counts) {
    std::vector<Packet> out;
    auto at = [](const std::vector<int>& v, size_t i) { return i < v.size() ? v[i] : 0; };
    if (m.vid == kVidNollie && m.pid == 0x1F11 && (previous.empty() || at(previous, 0) != at(counts, 0))) {
        Packet p;
        p.bytes.assign(kFsReport, 0);
        p.bytes[1] = 0xFE;
        p.bytes[2] = 0x03;
        p.bytes[3] = static_cast<uint8_t>(at(counts, 0) & 0xFF);
        p.bytes[4] = static_cast<uint8_t>((at(counts, 0) >> 8) & 0xFF);
        if (at(counts, 0) > 0) out.push_back(std::move(p));
    }
    if (m.vid == kVidNollieHs && m.pid == 0x4714 && at(counts, 26) > 0) {
        const bool longChain = at(counts, 26) > 20;
        if (previous.empty() || at(previous, 26) == 0 || (at(previous, 26) > 20) != longChain) {
            Packet p;
            p.bytes.assign(kFsReport, 0);
            p.bytes[1] = 0x80;
            p.bytes[2] = longChain ? 0 : 1;
            out.push_back(std::move(p));
        }
    }
    return out;
}

}  // namespace luma::app::nollie
