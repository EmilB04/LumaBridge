// OpenRGB's SDK server protocol (TCP, 127.0.0.1:6742 by default), as OpenRGB documents it
// (NetworkProtocol.md). LumaBridge is a client: it lists OpenRGB's devices and sends every
// LED's color, so every device OpenRGB supports can follow LumaBridge. Pure packet building
// and parsing, tested; the socket lives in openrgb_output.cpp.
//
// Every packet: "ORGB", then little-endian u32 device index, u32 packet ID, u32 data size,
// then the data. LumaBridge asks for protocol version 1 (it adds the vendor string to the
// controller data) and parses only what it needs; each controller's data starts with its
// own size, so one it can't parse is skipped, not fatal.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "color.h"
#include "lighting_identity.h"

namespace luma::app::openrgb {

constexpr uint16_t kDefaultPort = 6742;
constexpr uint32_t kProtocolVersion = 1;  // what LumaBridge asks for
constexpr size_t kHeaderSize = 16;

enum PacketId : uint32_t {
    kRequestControllerCount = 0,
    kRequestControllerData = 1,
    kRequestProtocolVersion = 40,
    kSetClientName = 50,
    kDeviceListUpdated = 100,
    kUpdateLeds = 1050,
    kSetCustomMode = 1100,
    kUpdateMode = 1101,
};

// OpenRGB's device types (for showing them).
inline const char* TypeName(int type) {
    switch (type) {
    case 0: return "Motherboard";
    case 1: return "Memory";
    case 2: return "Graphics card";
    case 3: return "Cooler";
    case 4: return "LED strip";
    case 5: return "Keyboard";
    case 6: return "Mouse";
    case 7: return "Mousemat";
    case 8: return "Headset";
    case 9: return "Headset stand";
    case 10: return "Gamepad";
    case 11: return "Light";
    case 12: return "Speaker";
    default: return "Device";
    }
}

using Bytes = std::vector<uint8_t>;

inline void PutU16(Bytes* b, uint16_t v) {
    b->push_back(static_cast<uint8_t>(v));
    b->push_back(static_cast<uint8_t>(v >> 8));
}
inline void PutU32(Bytes* b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b->push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline Bytes Packet(uint32_t device, uint32_t id, const Bytes& data = {}) {
    Bytes b{'O', 'R', 'G', 'B'};
    PutU32(&b, device);
    PutU32(&b, id);
    PutU32(&b, static_cast<uint32_t>(data.size()));
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

struct Header {
    uint32_t device = 0, id = 0, size = 0;
};
inline std::optional<Header> ParseHeader(const uint8_t* p, size_t n) {
    if (n < kHeaderSize || std::memcmp(p, "ORGB", 4) != 0) return std::nullopt;
    auto u32 = [&](size_t at) {
        return static_cast<uint32_t>(p[at] | p[at + 1] << 8 | p[at + 2] << 16) | static_cast<uint32_t>(p[at + 3]) << 24;
    };
    return Header{u32(4), u32(8), u32(12)};
}

inline Bytes ClientName(const std::string& name) {
    Bytes b(name.begin(), name.end());
    b.push_back(0);
    return b;
}
inline Bytes U32Data(uint32_t v) {
    Bytes b;
    PutU32(&b, v);
    return b;
}

// OpenRGB's color: R | G << 8 | B << 16.
inline uint32_t ToColor(Rgb c) { return c.r | static_cast<uint32_t>(c.g) << 8 | static_cast<uint32_t>(c.b) << 16; }

// UpdateLEDs data: u32 size (of all of it), u16 count, the colors.
inline Bytes UpdateLeds(const std::vector<Rgb>& colors) {
    Bytes b;
    const uint32_t size = static_cast<uint32_t>(4 + 2 + 4 * colors.size());
    PutU32(&b, size);
    PutU16(&b, static_cast<uint16_t>(colors.size()));
    for (const Rgb& c : colors) PutU32(&b, ToColor(c));
    return b;
}

struct Zone {
    std::string name;
    uint32_t leds = 0;
};

struct Controller {
    int type = -1;
    std::string name, vendor, description, location, serial;
    std::string id;  // assigned on enumeration; used for settings and mode restoration
    std::vector<Zone> zones;
    uint32_t leds = 0;  // colors it takes in UpdateLEDs
    int activeMode = -1;
    // The active mode exactly as OpenRGB sent it: sent back with UpdateMode to give the
    // device its own effect again when LumaBridge lets go.
    Bytes activeModeData;
};

namespace detail {
struct Reader {
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;
    bool Need(size_t k) {
        if (!ok || at > n || k > n - at) ok = false;
        return ok;
    }
    uint16_t U16() {
        if (!Need(2)) return 0;
        const uint16_t v = static_cast<uint16_t>(p[at] | p[at + 1] << 8);
        at += 2;
        return v;
    }
    uint32_t U32() {
        if (!Need(4)) return 0;
        const uint32_t v = static_cast<uint32_t>(p[at] | p[at + 1] << 8 | p[at + 2] << 16) | static_cast<uint32_t>(p[at + 3]) << 24;
        at += 4;
        return v;
    }
    std::string Str() {  // u16 length (with the terminating 0), the characters
        const uint16_t len = U16();
        if (!Need(len)) return {};
        std::string s(reinterpret_cast<const char*>(p + at), len);
        at += len;
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
    void Skip(size_t k) {
        if (Need(k)) at += k;
    }
};

// One mode (protocol versions 0-2): name, value, flags, speed min/max, colors min/max, speed,
// direction, color mode, colors.
inline void SkipMode(Reader* r) {
    r->Str();
    r->Skip(4 * 9);
    const uint16_t colors = r->U16();
    r->Skip(4u * colors);
}
}  // namespace detail

// The data of RequestControllerData (protocol version `version`, 0-2), from its size field on.
inline std::optional<Controller> ParseController(const uint8_t* p, size_t n, uint32_t version = kProtocolVersion) {
    if (version > 2 || !p) return std::nullopt;
    detail::Reader r{p, n};
    const uint32_t size = r.U32();
    if (!r.ok || size < 4 || size > n) return std::nullopt;
    r.n = size;
    Controller c;
    c.type = static_cast<int>(r.U32());
    c.name = r.Str();
    if (version >= 1) c.vendor = r.Str();
    c.description = r.Str();
    r.Str();  // version
    c.serial = r.Str();
    c.location = r.Str();
    const uint16_t modes = r.U16();
    c.activeMode = static_cast<int>(r.U32());
    for (uint16_t i = 0; i < modes && r.ok; ++i) {
        const size_t start = r.at;
        detail::SkipMode(&r);
        if (static_cast<int>(i) == c.activeMode && r.ok) c.activeModeData.assign(p + start, p + r.at);
    }
    const uint16_t zones = r.U16();
    for (uint16_t i = 0; i < zones && r.ok; ++i) {
        Zone z;
        z.name = r.Str();
        r.U32();  // type
        r.U32();  // leds min
        r.U32();  // leds max
        z.leds = r.U32();
        // A matrix: its length field, then height, width and the map. Readers only check the
        // length for 0, and writers differ on what it counts, so the size comes from h * w.
        if (r.U16()) {
            const uint32_t h = r.U32(), w = r.U32();
            if (h > 4096 || w > 4096) r.ok = false;
            else r.Skip(4ull * h * w);
        }
        c.zones.push_back(z);
    }
    const uint16_t leds = r.U16();
    for (uint16_t i = 0; i < leds && r.ok; ++i) {
        r.Str();
        r.U32();
    }
    c.leds = r.U16();  // the colors that follow: one per LED
    r.Skip(4ull * c.leds);
    if (!r.ok) return std::nullopt;
    return c;
}

inline std::string Identity(const Controller& c) {
    return lighting::Identity({c.name, c.vendor, std::to_string(c.type), c.serial, c.location});
}

// UpdateMode data: u32 size (of all of it), i32 mode index, the mode.
inline Bytes UpdateMode(int index, const Bytes& mode) {
    Bytes b;
    PutU32(&b, static_cast<uint32_t>(8 + mode.size()));
    PutU32(&b, static_cast<uint32_t>(index));
    b.insert(b.end(), mode.begin(), mode.end());
    return b;
}

}  // namespace luma::app::openrgb
