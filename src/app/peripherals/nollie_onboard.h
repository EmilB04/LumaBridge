// What a Nollie hub keeps on its own: the effects it plays with no program running, and the
// boot effect it plays at power-on. These are the questions Nollie's own app (NollieRGB) asks
// the hub, and how it reads the answers. Only questions: nothing here changes the hub.
// Pure and tested.
//
// A question is a report of the usual size: the command, what is asked, and for some a
// number (which canvas). The answer is one input report; `reply` below is that report
// without its report ID.
//
// The hub's effects live on "canvases": a group of channels with one effect. Canvases 0-13
// loop while the hub is on its own; canvases 14 and 15 are the boot canvases, played once at
// power-on.
#pragma once

#include <cstdint>
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
                  kFrameMs = 6, kBootFrameMs = 7, kLedCounts = 8, kBootCanvasMap = 11, kBootPlaylist = 19;

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

}  // namespace luma::app::nollie::onboard
