// Per-device lighting and the "Your setup" canvas on the Lighting page.
//
// Every device follows the main look (Prefs' manual effect) unless it has its own. Games
// light every device alike; the per-device looks show whenever LumaBridge shows your own
// lighting (Manual mode, or Auto mode's "your color" between games). The canvas shows each
// device with its live effect where you put it. Pure, tested.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "color.h"
#include "effects.h"

namespace luma::app {

// An effect with its settings: what the Lighting page edits.
struct Look {
    fx::Kind effect = fx::Kind::Static;
    Rgb color1{0, 140, 255};
    Rgb color2{255, 255, 255};  // second color of gradient / comet / twinkle
    float speedHz = 0.5f;       // cycles per second (0 = still)
    float hueStart = 0, hueSpan = 360, saturation = 1;  // rainbows
    int spread = 1;                                    // rainbow wave: rainbows around each fan
    bool reverse = false;
    bool operator==(const Look& o) const {
        return effect == o.effect && color1 == o.color1 && color2 == o.color2 && speedHz == o.speedHz &&
               hueStart == o.hueStart && hueSpan == o.hueSpan && saturation == o.saturation && spread == o.spread &&
               reverse == o.reverse;
    }
    bool operator!=(const Look& o) const { return !(*this == o); }
};

inline fx::Params ToParams(const Look& l) {
    fx::Params p;
    p.kind = l.effect;
    p.color1 = l.color1;
    p.color2 = l.color2;
    p.speed = l.effect == fx::Kind::Static ? 0 : l.speedHz;
    p.hueStart = l.hueStart;
    p.hueSpan = l.hueSpan;
    p.saturation = l.saturation;
    p.spread = l.spread;
    p.reverse = l.reverse;
    return p;
}

// The devices, in the order the Lighting page lists them.
namespace device {
constexpr const char* kFans = "fans";          // fans on the ARGB header (Aura)
constexpr const char* kBoard = "board";        // the motherboard's own LEDs (Aura)
constexpr const char* kRam = "ram";            // HyperX / Kingston FURY memory
constexpr const char* kMouse = "mouse";        // Logitech (G HUB / HID++)
constexpr const char* kKeyboard = "keyboard";  // ASUS ROG Azoth
constexpr const char* kOther = "other";        // devices lit through OpenRGB
inline const std::vector<const char*>& All() {
    static const std::vector<const char*> kAll{kFans, kBoard, kRam, kMouse, kKeyboard, kOther};
    return kAll;
}
inline const char* Name(const std::string& id) {
    if (id == kFans) return "Fans";
    if (id == kBoard) return "Motherboard";
    if (id == kRam) return "Memory";
    if (id == kMouse) return "Mouse";
    if (id == kKeyboard) return "Keyboard";
    if (id == kOther) return "Other devices";
    return "";
}
}  // namespace device

struct DeviceLighting {
    bool own = false;  // false: follows the main look
    Look look;
    // The device's own brightness (0..1), on top of the overall one. Always applies, games
    // included, whether or not it has its own look.
    float brightness = 1;
    // Moving effects run the other way on this device (its own look, the main one and games
    // alike), for devices whose LEDs are numbered the other way round or mounted mirrored.
    bool reverse = false;
    // Handed back to the device's own app (Armoury Crate, G HUB, ...): LumaBridge leaves it
    // alone, games included.
    bool native = false;
};

// The app that lights a device when LumaBridge hands it back.
inline const char* NativeApp(const std::string& id) {
    if (id == device::kMouse) return "G HUB";
    if (id == device::kOther) return "its own app";
    return "Armoury Crate";
}

// "own|effect|RRGGBB|RRGGBB|speed|hueStart|hueSpan|saturation|spread|reverse|brightness|flip|native"
// (0.7.0 wrote the first ten only, 0.14.x the first eleven). `reverse` is the look's own;
// `flip` is the device's direction (DeviceLighting::reverse); `native`: handed back.
inline std::string EncodeDevice(const DeviceLighting& d) {
    const Look& l = d.look;
    char buf[160];
    snprintf(buf, sizeof buf, "%d|%d|%02X%02X%02X|%02X%02X%02X|%g|%g|%g|%g|%d|%d|%g|%d|%d", d.own ? 1 : 0,
             static_cast<int>(l.effect), l.color1.r, l.color1.g, l.color1.b, l.color2.r, l.color2.g, l.color2.b,
             l.speedHz, l.hueStart, l.hueSpan, l.saturation, l.spread, l.reverse ? 1 : 0, d.brightness,
             d.reverse ? 1 : 0, d.native ? 1 : 0);
    return buf;
}

inline bool DecodeDevice(const std::string& s, DeviceLighting* out) {
    std::vector<std::string> f;
    for (size_t pos = 0;;) {
        const size_t bar = s.find('|', pos);
        f.push_back(s.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos));
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    if (f.size() < 10 || f.size() > 13) return false;
    auto hex = [](const std::string& h, Rgb* c) {
        if (h.size() != 6) return false;
        char* end = nullptr;
        const unsigned long v = std::strtoul(h.c_str(), &end, 16);
        if (*end) return false;
        *c = Rgb{static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
        return true;
    };
    DeviceLighting d;
    d.own = f[0] == "1";
    const int kind = std::atoi(f[1].c_str());
    if (kind < 0 || kind > static_cast<int>(fx::Kind::Twinkle)) return false;
    d.look.effect = static_cast<fx::Kind>(kind);
    if (!hex(f[2], &d.look.color1) || !hex(f[3], &d.look.color2)) return false;
    auto num = [](const std::string& x, float lo, float hi, float* v) {
        const float n = static_cast<float>(std::atof(x.c_str()));
        if (n < lo || n > hi) return false;
        *v = n;
        return true;
    };
    if (!num(f[4], 0, 10, &d.look.speedHz) || !num(f[5], 0, 360, &d.look.hueStart) ||
        !num(f[6], 10, 360, &d.look.hueSpan) || !num(f[7], 0, 1, &d.look.saturation))
        return false;
    d.look.spread = std::atoi(f[8].c_str());
    if (d.look.spread < 1 || d.look.spread > 4) d.look.spread = 1;
    d.look.reverse = f[9] == "1";
    if (f.size() >= 11 && !num(f[10], 0, 1, &d.brightness)) d.brightness = 1;
    d.reverse = f.size() >= 12 && f[11] == "1";
    d.native = f.size() >= 13 && f[12] == "1";
    *out = d;
    return true;
}

// ---- The canvas ---------------------------------------------------------------------

// Where an item sits on the canvas: its center, 0..1 across and down. Items: "fan0".."fan7"
// (one per fan on the ARGB header), "board", "ram", "mouse", "keyboard".
struct Spot {
    float x = 0.5f, y = 0.5f;
    bool operator==(const Spot& o) const { return x == o.x && y == o.y; }
};

inline std::string FanItem(int i) { return "fan" + std::to_string(i); }

// The starting layout: a case on the left (fans stacked at the front, the board and memory
// next to them) and the desk below (keyboard, mouse to its right).
inline Spot DefaultSpot(const std::string& item) {
    if (item.rfind("fan", 0) == 0) {
        const int i = std::atoi(item.c_str() + 3);
        return Spot{0.08f + 0.12f * static_cast<float>(i / 3), 0.16f + 0.26f * static_cast<float>(i % 3)};
    }
    if (item == device::kBoard) return Spot{0.42f, 0.30f};
    if (item == device::kRam) return Spot{0.62f, 0.30f};
    if (item == device::kKeyboard) return Spot{0.48f, 0.80f};
    if (item == device::kMouse) return Spot{0.82f, 0.78f};
    return Spot{};
}

inline Spot ClampSpot(Spot s) {
    s.x = s.x < 0 ? 0 : s.x > 1 ? 1 : s.x;
    s.y = s.y < 0 ? 0 : s.y > 1 ? 1 : s.y;
    return s;
}

// "x,y"
inline std::string EncodeSpot(Spot s) {
    char buf[48];
    snprintf(buf, sizeof buf, "%.4f,%.4f", s.x, s.y);
    return buf;
}
inline bool DecodeSpot(const std::string& s, Spot* out) {
    const size_t comma = s.find(',');
    if (comma == std::string::npos) return false;
    *out = ClampSpot(Spot{static_cast<float>(std::atof(s.substr(0, comma).c_str())),
                          static_cast<float>(std::atof(s.substr(comma + 1).c_str()))});
    return true;
}

}  // namespace luma::app
