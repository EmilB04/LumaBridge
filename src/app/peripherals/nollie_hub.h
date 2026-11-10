// Settings of a whole Nollie hub, beside its channels (nollie_layout.h): an effect that
// travels from channel to channel, and an effect that plays when LumaBridge starts lighting
// the hub. Pure and tested.
//
// The hub sits on an internal USB header and only shows what a program sends it; before
// Windows and LumaBridge are up it is dark. So the "boot" effect plays when LumaBridge
// starts (with Windows, if LumaBridge starts with it), when the PC wakes and the hub shows up
// again, or on request.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "device_lighting.h"
#include "nollie_sequence.h"

namespace luma::app::nollie {

enum class Travel { Off, Forward, Backward };

struct Hub {
    Travel travel = Travel::Off;  // the effect runs through the channels in order
    float travelSeconds = 1.5f;   // from the first channel to the last
    bool smooth = true;           // the effect flows LED by LED, not a whole fan at a time
    int refreshHz = 60;           // frames a second sent to the hub: 30 or 60
    SequenceParams sequence;      // a sequence across the parts instead of the lighting
    Gradient gradient;            // colors along the parts, in place of the effect's main color
    bool boot = false;            // play bootLook when the hub starts being lit
    float bootSeconds = 4.f;
    Look bootLook{fx::Kind::RainbowWave, Rgb{0, 140, 255}, Rgb{255, 255, 255}, 0.5f, 0, 360, 1, 1, false};

    bool operator==(const Hub& o) const {
        return travel == o.travel && travelSeconds == o.travelSeconds && smooth == o.smooth &&
               refreshHz == o.refreshHz && sequence == o.sequence && gradient == o.gradient && boot == o.boot &&
               bootSeconds == o.bootSeconds && bootLook == o.bootLook;
    }
};

// Used by the output planner and boot preview so duration and one-shot playback agree.
inline fx::Params BootEffect(const Hub& hub, int index, int parts) {
    fx::Params effect = ToParams(hub.bootLook);
    effect.oneShot = true;
    effect.parts = std::max(1, parts);
    effect.part = hub.travel == Travel::Backward ? effect.parts - 1 - index : index;
    if (fx::IsSettlingEffect(effect.kind)) effect.speed = effect.parts / std::max(0.1, hub.bootSeconds * 0.6);
    return effect;
}

// How far along the travel the part at `index` of `count` is (0 = first to show the effect,
// 1 = last). 0 for everything when the effect doesn't travel or there is one part.
inline double TravelOffset(Travel travel, int index, int count) {
    if (travel == Travel::Off || count < 2 || index < 0 || index >= count) return 0;
    const double along = static_cast<double>(index) / (count - 1);
    return travel == Travel::Forward ? along : 1.0 - along;
}

// Where a part's first LED sits along the travel, and how far its LEDs run behind it, for an
// effect taking `seconds` from the first LED of the first part to the last LED of the last.
// Fan by fan (`smooth` off): each part one step (TravelOffset), no lag. Smooth: every LED its
// own place, each part 1/count of the way.
struct TravelPlace {
    double offset = 0;  // for Flowed
    double lag = 0;     // Segment::lag, seconds
};
inline TravelPlace Place(Travel travel, bool smooth, int index, int count, double seconds) {
    if (!smooth || travel == Travel::Off || count < 1 || index < 0 || index >= count)
        return {TravelOffset(travel, index, count), 0};
    const double step = 1.0 / count;
    if (travel == Travel::Forward) return {index * step, step * seconds};
    return {1.0 - index * step, -step * seconds};
}

inline int ClampRefresh(int hz) { return hz >= 45 ? 60 : 30; }

// "travel|travelSeconds|boot|bootSeconds|smooth|refreshHz|sequence|partSeconds|RRGGBB|RRGGBB|
//  gradient on|RRGGBB,RRGGBB,...|flow|<look>", the look as Lighting's device encoding.
namespace hubdetail {
inline std::string Hex(Rgb c) {
    static const char* k = "0123456789ABCDEF";
    std::string s;
    for (uint8_t v : {c.r, c.g, c.b}) {
        s += k[v >> 4];
        s += k[v & 15];
    }
    return s;
}
inline bool FromHex(const std::string& h, Rgb* c) {
    if (h.size() != 6) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(h.c_str(), &end, 16);
    if (*end) return false;
    *c = Rgb{static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    return true;
}
}  // namespace hubdetail

inline std::string Encode(const Hub& h) {
    using hubdetail::Hex;
    DeviceLighting d;
    d.own = true;
    d.look = h.bootLook;
    std::string stops;
    for (const Rgb& c : h.gradient.stops) stops += (stops.empty() ? "" : ",") + Hex(c);
    return std::to_string(static_cast<int>(h.travel)) + "|" + std::to_string(h.travelSeconds) + "|" + (h.boot ? "1" : "0") +
           "|" + std::to_string(h.bootSeconds) + "|" + (h.smooth ? "1" : "0") + "|" + std::to_string(h.refreshHz) + "|" +
           std::to_string(static_cast<int>(h.sequence.kind)) + "|" + std::to_string(h.sequence.partSeconds) + "|" +
           Hex(h.sequence.color1) + "|" + Hex(h.sequence.color2) + "|" + (h.gradient.on ? "1" : "0") + "|" + stops + "|" +
           std::to_string(h.gradient.flow) + "|" + EncodeDevice(d);
}

inline bool Decode(const std::string& s, Hub* out) {
    size_t pos = 0;
    std::string f[13];
    for (std::string& field : f) {
        const size_t bar = s.find('|', pos);
        if (bar == std::string::npos) return false;
        field = s.substr(pos, bar - pos);
        pos = bar + 1;
    }
    const int travel = settings::Integer(f[0]);
    if (travel < 0 || travel > 2) return false;
    DeviceLighting d;
    if (!DecodeDevice(s.substr(pos), &d, true)) return false;
    Hub h;
    h.travel = static_cast<Travel>(travel);
    h.travelSeconds = settings::Clamped(f[1], h.travelSeconds, 0.2f, 5.f);
    h.boot = f[2] == "1";
    h.bootSeconds = settings::Clamped(f[3], h.bootSeconds, 1.f, 30.f);
    h.smooth = f[4] != "0";
    h.refreshHz = ClampRefresh(settings::Integer(f[5]));
    const int seq = settings::Integer(f[6]);
    h.sequence.kind = seq >= 0 && seq < kSequences ? static_cast<Sequence>(seq) : Sequence::None;
    h.sequence.partSeconds = settings::Clamped(f[7], h.sequence.partSeconds, 0.1, 10.0);
    if (!hubdetail::FromHex(f[8], &h.sequence.color1) || !hubdetail::FromHex(f[9], &h.sequence.color2)) return false;
    h.gradient.on = f[10] == "1";
    h.gradient.stops.clear();
    for (size_t pos = 0; pos <= f[11].size() && h.gradient.stops.size() < kMaxGradientStops;) {
        const size_t comma = f[11].find(',', pos);
        Rgb c;
        if (!hubdetail::FromHex(f[11].substr(pos, comma == std::string::npos ? std::string::npos : comma - pos), &c)) return false;
        h.gradient.stops.push_back(c);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (h.gradient.stops.size() < 2) return false;
    h.gradient.flow = settings::Clamped(f[12], h.gradient.flow, -2.0, 2.0);
    h.bootLook = d.look;
    *out = h;
    return true;
}

}  // namespace luma::app::nollie
