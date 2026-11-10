// What a Nollie hub keeps on its own: the effects it plays with no program running, and the
// boot effect it plays at power-on. These are the questions Nollie's own app (NollieRGB) asks
// the hub, and how it reads the answers; and, at the end, the one thing LumaBridge changes:
// the boot effect (BootSetup and the reports that write it). Pure and tested.
//
// A question is a report of the usual size: the command, what is asked, and for some a
// number (which canvas). The answer is one input report; `reply` below is that report
// without its report ID.
//
// The hub's effects live on "canvases": a group of channels with one effect. Canvases 0-13
// loop while the hub is on its own; canvases 14 and 15 are the boot canvases, played once at
// power-on.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "color.h"
#include "nollie_protocol.h"

namespace luma::app::nollie::onboard {

constexpr uint8_t kGetConfig = 0xFC, kGetEffect = 0xF9;

// What is asked, after kGetConfig.
constexpr uint8_t kConfigVersion = 1, kConfigFirmware = 2;
// What is asked, after kGetEffect.
constexpr uint8_t kMode = 1, kCanvasEffect = 2, kCanvasMap = 3, kCanvasCount = 4, kBootCanvasCount = 5,
                  kFrameMs = 6, kBootFrameMs = 7, kLedCounts = 8, kBootCanvasMap = 11, kBootPlaylist = 19, kFrameDelay = 18;

constexpr int kLoopCanvases = 14;  // canvases 0-13
constexpr int kBootCanvasBase = 14, kBootCanvases = 2;

// Hubs asked: the 64-byte hubs with the OS2 firmware. The others may know the same questions,
// but that isn't confirmed anywhere.
inline bool Speaks(const Model& m) { return m.vid == kVidNollieOs2 && m.family == Family::Fs; }

// The report asking `what`. `number`: the canvas, or 0 for the channel maps (the place in the
// map to read from); -1 for questions without one.
inline Packet Query(uint8_t command, uint8_t what, int number = -1) {
    Packet p;
    p.bytes.assign(kFsReport, 0);
    p.bytes[1] = command;
    p.bytes[2] = what;
    if (number >= 0) p.bytes[3] = static_cast<uint8_t>(number);
    return p;
}

inline int At(const std::vector<uint8_t>& reply, size_t i) { return i < reply.size() ? reply[i] : 0; }

// A hub that doesn't know a question answers with zeros (or not at all).
inline bool Empty(const std::vector<uint8_t>& reply) {
    for (uint8_t b : reply)
        if (b) return false;
    return true;
}

struct Version {
    int settings = 0;  // the version of the hub's settings layout
    int board = 0;     // the board's number (8027: an 8 channel Plus)
};
inline Version DecodeVersion(const std::vector<uint8_t>& reply) {
    return {At(reply, 1), At(reply, 2) | (At(reply, 3) << 8)};
}

inline std::string DecodeFirmware(const std::vector<uint8_t>& reply) {
    return std::to_string(At(reply, 0)) + "." + std::to_string(At(reply, 1)) + "." + std::to_string(At(reply, 2));
}

// Who lights the hub, and (firmware 2.4.0 on) what it is doing right now.
inline const char* ModeName(int mode) {
    switch (mode) {
    case 0: return "lit by a program";
    case 1: return "its own effects";
    case 2: return "mode 2";
    case 3: return "a saved animation";
    default: return "an unknown mode";
    }
}
inline const char* StageName(int stage) {
    switch (stage) {
    case 0: return "not told";
    case 1: return "playing the boot effect";
    case 2: return "playing its own effects";
    case 3: return "lit by a program";
    case 4: return "asleep with the PC";
    case 5: return "saving an animation";
    case 6: return "counting LEDs";
    default: return "something unknown";
    }
}

// What plays at power-on, then what stays.
inline const char* IntroName(int intro) {
    switch (intro) {
    case 0: return "the boot canvases";
    case 1: return "nothing";
    case 2: return "the saved animation once";
    default: return "unknown";
    }
}
inline const char* SteadyName(int steady) {
    switch (steady) {
    case 0: return "its own effects";
    case 1: return "the saved animation";
    default: return "unknown";
    }
}

// The effect on a canvas, by the number the hub keeps. Boot canvases count in their own list.
inline const char* EffectName(int mode, bool boot) {
    static const char* const loop[] = {"Off", "Solid", "Breath", "Scanning", "Rainbow", "Rainbow cycle",
                                       "Meteor", "Shake", "Slowly wipe", "Color wipe", "Rainbow strip",
                                       "Rainbow strip l", "Wave", "Sunny", "Pull flower", "Stackab1",
                                       "Stackab2", "Stackab3", "Integration1", "Integration2",
                                       "Interchange", "Bullets", "Sequence", "ColorFromPalette"};
    static const char* const start[] = {"Light up", "Color wipe", "Slowly wipe", "Pull flower",
                                        "Stackab1", "Stackab2", "Stackab3", "Bullets"};
    if (mode < 0) return "unknown";
    if (boot) return mode < 8 ? start[mode] : "unknown";
    return mode < 24 ? loop[mode] : "unknown";
}

struct CanvasEffect {
    int mode = 0;        // EffectName
    int brightness = 0;
    int step = 0;        // speed
    int size = 0;
    int channels = 0;    // how many channels of the map belong to this canvas
    int map = 0;         // how the LED order is used
    int delay = 0;
    Rgb colors[3];       // in the order the hub sends them; read as R, G, B

    bool operator==(const CanvasEffect& o) const {
        return mode == o.mode && brightness == o.brightness && step == o.step && size == o.size &&
               channels == o.channels && map == o.map && delay == o.delay && colors[0] == o.colors[0] &&
               colors[1] == o.colors[1] && colors[2] == o.colors[2];
    }
};

// The answer repeats the question (three bytes), then the canvas. False for an answer too
// short to hold one.
inline bool DecodeCanvasEffect(const std::vector<uint8_t>& reply, CanvasEffect* out) {
    if (reply.size() < 19) return false;
    CanvasEffect c;
    c.mode = reply[3];
    c.brightness = reply[4];
    c.step = reply[5];
    c.size = reply[6];
    c.channels = reply[7];
    c.map = reply[8];
    c.delay = reply[9];
    for (int i = 0; i < 3; ++i) c.colors[i] = Rgb{reply[10 + i * 3], reply[11 + i * 3], reply[12 + i * 3]};
    *out = c;
    return true;
}

// LEDs on each channel as the hub counts them: two bytes a channel, low byte first.
inline std::vector<int> DecodeLedCounts(const std::vector<uint8_t>& reply, int channels) {
    std::vector<int> out;
    for (int c = 0; c < channels; ++c) out.push_back(At(reply, c * 2) | (At(reply, c * 2 + 1) << 8));
    return out;
}

// The channels in canvas order: the first canvas's channels, then the next one's.
inline std::vector<int> DecodeChannelMap(const std::vector<uint8_t>& reply, int channels) {
    std::vector<int> out;
    for (int c = 0; c < channels; ++c) out.push_back(At(reply, c));
    return out;
}

// "1, 2, 3" for the log.
inline std::string List(const std::vector<int>& values) {
    std::string s;
    for (size_t i = 0; i < values.size(); ++i) s += (i ? ", " : "") + std::to_string(values[i]);
    return s;
}

// The first `count` bytes as "0A 1B ..." for the log, so an answer read wrongly can still be
// read by eye.
inline std::string Hex(const std::vector<uint8_t>& reply, size_t count) {
    static const char digits[] = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < count && i < reply.size(); ++i) {
        if (i) s += ' ';
        s += digits[reply[i] >> 4];
        s += digits[reply[i] & 15];
    }
    return s;
}

// ---- Saving the boot effect ----------------------------------------------------------
//
// The hub keeps up to two boot canvases (14 and 15). LumaBridge uses the first one for every
// channel that has something on it and tells the hub there is one boot canvas. The hub saves
// what it was told a few seconds after the last report, and plays it at power-on with no
// program running.

constexpr uint8_t kSetEffect = 0xFA;
constexpr uint8_t kSetCanvasEffect = 2, kSetBootCanvasCount = 5, kSetLedCounts = 8, kSetBootCanvasMap = 11,
                  kSetBootPlaylist = 19, kSetMode = 1, kPlayBoot = 32, kSetBootFrameMs = 7, kSetFrameDelay = 18;
constexpr int kMapBytes = 32;  // the hub's channel maps are always this long
constexpr int kBootEffects = 8;

struct BootSetup {
    int effect = 0;       // EffectName(effect, true)
    int brightness = 100; // 1-100
    int step = 3;         // speed, 1-10
    int size = 30;        // 0-50
    int delay = 0;        // 0-8
    int pause = 33;       // the pause between sweeps, 1-255 (the hub's "frame delay")
    Rgb colors[3]{Rgb{255, 91, 3}, Rgb{0, 0, 0}, Rgb{0, 0, 0}};

    bool operator==(const BootSetup& o) const {
        return effect == o.effect && brightness == o.brightness && step == o.step && size == o.size &&
               delay == o.delay && pause == o.pause && colors[0] == o.colors[0] && colors[1] == o.colors[1] &&
               colors[2] == o.colors[2];
    }
    bool operator!=(const BootSetup& o) const { return !(*this == o); }
};

// Only the ranges the hub's own app offers are sent.
inline BootSetup Clamped(BootSetup b) {
    b.effect = std::clamp(b.effect, 0, kBootEffects - 1);
    b.brightness = std::clamp(b.brightness, 1, 100);
    b.step = std::clamp(b.step, 1, 10);
    b.size = std::clamp(b.size, 0, 50);
    b.delay = std::clamp(b.delay, 0, 8);
    b.pause = std::clamp(b.pause, 1, 255);
    return b;
}

// What the effects use (from Nollie's own app): "Light up" only the first color, brightness
// and speed; the others everything.
inline bool UsesAll(const BootSetup& b) { return b.effect != 0; }

// "effect|brightness|step|size|RRGGBB|RRGGBB|RRGGBB|delay|pause" for the settings file.
inline std::string Encode(const BootSetup& b) {
    static const char digits[] = "0123456789ABCDEF";
    std::string s = std::to_string(b.effect) + "|" + std::to_string(b.brightness) + "|" + std::to_string(b.step) + "|" +
                    std::to_string(b.size);
    const std::string tail = "|" + std::to_string(b.delay) + "|" + std::to_string(b.pause);
    for (const Rgb& c : b.colors) {
        s += '|';
        for (uint8_t v : {c.r, c.g, c.b}) {
            s += digits[v >> 4];
            s += digits[v & 15];
        }
    }
    return s + tail;
}

// Settings saved before delay and pause existed have seven fields; they read with the defaults.
inline bool Decode(const std::string& text, BootSetup* out) {
    std::string f[9];
    size_t pos = 0;
    int fields = 0;
    for (; fields < 9 && pos <= text.size(); ++fields) {
        const size_t bar = text.find('|', pos);
        f[fields] = text.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
        pos = bar == std::string::npos ? text.size() + 1 : bar + 1;
    }
    if (fields != 7 && fields != 9) return false;
    auto number = [](const std::string& s, int* v) {
        char* end = nullptr;
        const long n = std::strtol(s.c_str(), &end, 10);
        if (s.empty() || *end) return false;
        *v = static_cast<int>(n);
        return true;
    };
    BootSetup b;
    if (!number(f[0], &b.effect) || !number(f[1], &b.brightness) || !number(f[2], &b.step) || !number(f[3], &b.size))
        return false;
    for (int i = 0; i < 3; ++i) {
        char* end = nullptr;
        const std::string& h = f[4 + i];
        const unsigned long v = std::strtoul(h.c_str(), &end, 16);
        if (h.size() != 6 || *end) return false;
        b.colors[i] = Rgb{static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    }
    if (fields == 9 && (!number(f[7], &b.delay) || !number(f[8], &b.pause))) return false;
    *out = Clamped(b);
    return true;
}

// Frame times the hub offers for its boot canvases (milliseconds a frame), fastest first.
constexpr int kFrameTimes[] = {8, 9, 10, 11, 12, 14, 16, 20, 25, 33, 50, 100};
inline int NearestFrameMs(int ms) {
    int best = kFrameTimes[0];
    for (int t : kFrameTimes)
        if (std::abs(t - ms) < std::abs(best - ms)) best = t;
    return best;
}

// A boot effect for the whole hub: one effect, or two with some channels on the second. The
// hub holds two boot canvases.
struct BootPlan {
    BootSetup first, second;
    uint32_t secondMask = 0;  // bit z: zone z plays `second`
    int frameMs = 16;         // the boot canvases' frame time, one of kFrameTimes

    bool operator==(const BootPlan& o) const {
        return first == o.first && second == o.second && secondMask == o.secondMask && frameMs == o.frameMs;
    }
};

// "frameMs;mask;first;second". Settings saved with the first effect only read as that.
inline std::string Encode(const BootPlan& p) {
    return std::to_string(p.frameMs) + ";" + std::to_string(p.secondMask) + ";" + Encode(p.first) + ";" + Encode(p.second);
}
inline bool Decode(const std::string& text, BootPlan* out) {
    BootPlan p;
    if (text.find(';') == std::string::npos) {
        if (!Decode(text, &p.first)) return false;
        p.second = p.first;
        *out = p;
        return true;
    }
    std::string f[4];
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t semi = text.find(';', pos);
        if (i < 3 && semi == std::string::npos) return false;
        f[i] = text.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = semi == std::string::npos ? text.size() : semi + 1;
    }
    char* end = nullptr;
    const long ms = std::strtol(f[0].c_str(), &end, 10);
    if (f[0].empty() || *end) return false;
    const unsigned long mask = std::strtoul(f[1].c_str(), &end, 10);
    if (f[1].empty() || *end) return false;
    if (!Decode(f[2], &p.first) || !Decode(f[3], &p.second)) return false;
    p.frameMs = NearestFrameMs(static_cast<int>(ms));
    p.secondMask = static_cast<uint32_t>(mask);
    *out = p;
    return true;
}

// What goes to the hub: which channels play which canvas, and the order of the map (the first
// canvas's channels, then the second's, then the rest, so it stays every channel once).
struct BootLayout {
    std::vector<int> first, second, order;
    BootSetup firstSetup, secondSetup;
    int canvases = 1;
};
inline BootLayout Layout(const BootPlan& plan, std::vector<int> used, int channels) {
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    used.erase(std::remove_if(used.begin(), used.end(), [&](int c) { return c < 0 || c >= channels; }), used.end());
    BootLayout l;
    l.firstSetup = Clamped(plan.first);
    l.secondSetup = Clamped(plan.second);
    for (int z : used) (z < 32 && (plan.secondMask >> z & 1) ? l.second : l.first).push_back(z);
    if (l.first.empty()) {  // everything on the second effect: it is the only one
        l.first = l.second;
        l.second.clear();
        l.firstSetup = l.secondSetup;
    }
    l.canvases = l.second.empty() ? 1 : 2;
    l.order = l.first;
    l.order.insert(l.order.end(), l.second.begin(), l.second.end());
    for (int c = 0; c < channels; ++c)
        if (std::find(l.order.begin(), l.order.end(), c) == l.order.end()) l.order.push_back(c);
    return l;
}

// The channels that play the boot effect come first in the hub's map, then the rest, so the
// map stays a list of every channel once. `used`: zones with something on them.
inline std::vector<int> BootChannelOrder(std::vector<int> used, int channels) {
    return Layout(BootPlan{}, std::move(used), channels).order;
}

// LED counts to tell the hub: what it has now, with the used channels set to what is on them.
inline std::vector<int> MergeLedCounts(std::vector<int> current, const std::vector<int>& used, int channels) {
    current.resize(static_cast<size_t>(channels), 0);
    for (int c = 0; c < channels && c < static_cast<int>(used.size()); ++c)
        if (used[c] > 0) current[c] = used[c];
    return current;
}

inline Packet SetPacket(uint8_t what, const std::vector<uint8_t>& data) {
    Packet p;
    p.bytes.assign(kFsReport, 0);
    p.bytes[1] = kSetEffect;
    p.bytes[2] = what;
    for (size_t i = 0; i < data.size() && 3 + i < p.bytes.size(); ++i) p.bytes[3 + i] = data[i];
    return p;
}

inline Packet LedCountsPacket(const std::vector<int>& counts) {
    std::vector<uint8_t> d;
    for (int n : counts) {
        d.push_back(static_cast<uint8_t>(n & 0xFF));
        d.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    }
    return SetPacket(kSetLedCounts, d);
}

inline Packet BootCanvasCountPacket(int count) { return SetPacket(kSetBootCanvasCount, {static_cast<uint8_t>(count)}); }

// The offset byte (0) comes first: the whole map is always written.
inline Packet BootMapPacket(const std::vector<int>& order) {
    std::vector<uint8_t> d{0};
    for (int i = 0; i < kMapBytes; ++i) d.push_back(i < static_cast<int>(order.size()) ? static_cast<uint8_t>(order[i]) : 0);
    return SetPacket(kSetBootCanvasMap, d);
}

// A boot canvas (`which`: 0 or 1). `channels`: how many channels of the map play it; `map`: what
// the hub has now, kept as it is.
inline Packet BootCanvasPacket(const BootSetup& setup, int which, int channels, int map) {
    const BootSetup b = Clamped(setup);
    const int delay = b.delay;
    std::vector<uint8_t> d{static_cast<uint8_t>(kBootCanvasBase + which), static_cast<uint8_t>(b.effect),
                           static_cast<uint8_t>(b.brightness), static_cast<uint8_t>(b.step), static_cast<uint8_t>(b.size),
                           static_cast<uint8_t>(channels), static_cast<uint8_t>(map), static_cast<uint8_t>(delay)};
    for (const Rgb& c : b.colors) {
        d.push_back(c.r);
        d.push_back(c.g);
        d.push_back(c.b);
    }
    return SetPacket(kSetCanvasEffect, d);
}

// What plays at power-on (0: the boot canvases) and what stays after (0: the hub's own effects).
inline Packet BootPlaylistPacket(int intro, int steady) {
    return SetPacket(kSetBootPlaylist, {static_cast<uint8_t>(intro), static_cast<uint8_t>(steady)});
}

// The pauses between sweeps of all 16 canvases (the boot canvases are 14 and 15).
inline Packet FrameDelayPacket(const std::vector<int>& pauses) {
    std::vector<uint8_t> d;
    for (int i = 0; i < 16; ++i) d.push_back(static_cast<uint8_t>(std::clamp(i < static_cast<int>(pauses.size()) ? pauses[i] : 33, 1, 255)));
    return SetPacket(kSetFrameDelay, d);
}

inline Packet BootFrameMsPacket(int ms) { return SetPacket(kSetBootFrameMs, {static_cast<uint8_t>(NearestFrameMs(ms))}); }

// Who lights the hub: 0 a program (LumaBridge's colors), 1 its own effects.
inline Packet ModePacket(int mode) { return SetPacket(kSetMode, {static_cast<uint8_t>(mode)}); }

// Plays the boot effect (the hub has to be on its own effects, not showing a program's colors).
inline Packet PlayBootPacket() { return SetPacket(kPlayBoot, {}); }

}  // namespace luma::app::nollie::onboard
