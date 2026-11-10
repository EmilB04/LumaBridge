// What is plugged into a Nollie hub's channels, as the user sets it: nothing, an LED strip, or
// a chain of fans. A hub can't tell what is connected, so the lengths are the user's. Each
// channel is lit on its own: a strip runs the effect along its length, fans each show the
// whole pattern (or the pattern spans all of them). Pure and tested.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "effects.h"
#include "nollie_protocol.h"

namespace luma::app::nollie {

enum class Kind { Unused, Strip, Fans };

struct Channel {
    Kind kind = Kind::Unused;
    int fans = 3;           // Fans: how many are chained
    int ledsPerFan = 20;    // Fans: LEDs in one fan; Strip: its length
    bool repeatPerFan = true;  // Fans: each fan on its own (its own ring, look and place in a
                               // travelling effect); false: one pattern spans all the fans
    std::string name;       // the user's own ("Front fans"); empty: the hub's label

    // LEDs sent on this channel, within what the hub holds.
    int Leds(const Model& m) const {
        int n = 0;
        if (kind == Kind::Strip) n = ledsPerFan;
        if (kind == Kind::Fans) n = Fans() * LedsPerFan();
        return std::clamp(n, 0, m.maxLeds);
    }
    int Fans() const { return std::clamp(fans, 1, 16); }
    // The parts lit on their own: each fan when each fan is on its own, else the channel as one.
    int Segments() const { return kind == Kind::Fans && repeatPerFan ? Fans() : kind == Kind::Unused ? 0 : 1; }
    int LedsPerFan() const { return std::clamp(ledsPerFan, 1, 120); }
    bool operator==(const Channel& o) const {
        return kind == o.kind && fans == o.fans && ledsPerFan == o.ledsPerFan && repeatPerFan == o.repeatPerFan &&
               name == o.name;
    }
};

// The effect on one channel at `t` seconds, at `brightness` (0..1).
inline std::vector<Rgb> RenderChannel(const Channel& c, const Model& m, const fx::Params& effect, double t,
                                      double brightness) {
    std::vector<Rgb> out;
    const int n = c.Leds(m);
    if (n <= 0) return out;
    if (c.kind == Kind::Strip) {
        fx::RenderStrip(effect, t, n, &out);
    } else {
        fx::FanLayout layout;
        layout.fans = c.Fans();
        layout.ledsPerFan = c.LedsPerFan();
        layout.repeatPerFan = c.repeatPerFan;
        fx::RenderFans(effect, t, layout, &out, n);
    }
    if (brightness < 1.0)
        for (Rgb& p : out) p = Scale(p, brightness < 0 ? 0 : brightness);
    return out;
}

// One part of a channel (a fan, or the whole channel) at one moment of its effect.
struct Segment {
    fx::Params effect;
    double t = 0;           // seconds into the effect, at the part's first LED
    double brightness = 1;  // 0..1
    // Smooth travel: how many seconds the part's last LED runs behind its first (negative:
    // ahead), the LEDs in between in proportion, so an effect flows LED by LED instead of
    // jumping a whole part at a time. 0: every LED of the part at the same moment.
    double lag = 0;
};

// The channel with every part (Channel::Segments) showing its own effect: with each fan on its
// own, fan i shows segments[i] around its ring; otherwise the channel shows segments[0] along
// the strip or across all its fans. Missing parts stay dark.
inline std::vector<Rgb> RenderSegments(const Channel& c, const Model& m, const std::vector<Segment>& segments) {
    std::vector<Rgb> out;
    const int n = c.Leds(m);
    if (n <= 0 || segments.empty()) return out;
    out.assign(static_cast<size_t>(n), Rgb{});
    const bool eachFan = c.Segments() > 1;
    const int per = c.LedsPerFan();
    // A fan on its own is a ring of `per`; fans sharing one pattern a ring of all their LEDs
    // (as the Aura header's fans); a strip one line.
    const int ring = c.kind == Kind::Strip ? n : c.repeatPerFan ? per : c.Fans() * per;
    for (int i = 0; i < n; ++i) {
        const size_t part = eachFan ? static_cast<size_t>(i / per) : 0;
        if (part >= segments.size()) break;
        const Segment& s = segments[part];
        const int j = eachFan ? i % per : i;  // place within the part
        const int length = eachFan ? per : n;
        const double t = s.t - s.lag * j / length;
        // Twinkle has no shape to repeat: its sparkles go by the LED's place in the whole chain,
        // so the fans don't sparkle on the same LEDs at once.
        const Rgb color = s.effect.kind == fx::Kind::Twinkle ? fx::Render(s.effect, t, i, n)
                                                             : fx::Render(s.effect, t, i % ring, ring);
        out[static_cast<size_t>(i)] = s.brightness < 1.0 ? Scale(color, s.brightness < 0 ? 0 : s.brightness) : color;
    }
    return out;
}

// To find the right header: every fan its own color with its first LED white; a strip lit
// in one color with its first LED white.
inline std::vector<Rgb> TestChannel(const Channel& c, const Model& m) {
    std::vector<Rgb> out;
    const int n = c.Leds(m);
    if (n <= 0) return out;
    fx::FanLayout layout;
    layout.fans = c.kind == Kind::Strip ? 1 : c.Fans();
    layout.ledsPerFan = c.kind == Kind::Strip ? std::clamp(n, 1, 120) : c.LedsPerFan();
    fx::RenderFanTest(layout, &out, n);
    return out;
}

inline std::string DisplayName(const Channel& c, int zone) { return c.name.empty() ? ChannelName(zone) : c.name; }

// A channel's setting in the INI: "kind|fans|ledsPerFan|repeat|name" (the name last; "|" can't
// be in it).
inline std::string Encode(const Channel& c) {
    std::string name = c.name;
    for (char& ch : name)
        if (ch == '|' || ch == '\n' || ch == '\r') ch = ' ';
    return std::to_string(static_cast<int>(c.kind)) + "|" + std::to_string(c.fans) + "|" + std::to_string(c.ledsPerFan) +
           "|" + (c.repeatPerFan ? "1" : "0") + "|" + name;
}

inline bool Decode(const std::string& s, Channel* out) {
    std::vector<std::string> f;
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t bar = s.find('|', pos);
        if (bar == std::string::npos) return false;
        f.push_back(s.substr(pos, bar - pos));
        pos = bar + 1;
    }
    const int kind = std::atoi(f[0].c_str());
    if (kind < 0 || kind > 2) return false;
    Channel c;
    c.kind = static_cast<Kind>(kind);
    c.fans = std::clamp(std::atoi(f[1].c_str()), 1, 16);
    c.ledsPerFan = std::clamp(std::atoi(f[2].c_str()), 1, 630);
    c.repeatPerFan = f[3] != "0";
    c.name = s.substr(pos);
    *out = c;
    return true;
}

// The key of a channel in the settings (and, with "nollie." in front, of its lighting in
// Prefs::deviceLighting). `deviceId`: lighting::Identity of the hub.
inline std::string Key(const std::string& deviceId, int zone) { return deviceId + "." + std::to_string(zone); }
inline std::string LightingId(const std::string& deviceId, int zone) { return "nollie." + Key(deviceId, zone); }
// One fan of a channel, when each fan is on its own: its lighting in Prefs::deviceLighting.
inline std::string FanLightingId(const std::string& deviceId, int zone, int fan) {
    return LightingId(deviceId, zone) + ".f" + std::to_string(fan);
}

// Where each part sits along an effect travelling through the hub: the channels in use in
// order (`zones`, sorted), each fan of a channel with fans on their own a step of its own.
// Returns, per zone, the step of its first part, and the number of steps in `count`.
inline std::vector<std::pair<int, int>> TravelSteps(const std::vector<std::pair<int, Channel>>& zones, int* count) {
    std::vector<std::pair<int, int>> out;  // zone, first step
    int step = 0;
    for (const auto& [zone, c] : zones) {
        if (c.Segments() == 0) continue;
        out.emplace_back(zone, step);
        step += c.Segments();
    }
    if (count) *count = step;
    return out;
}

}  // namespace luma::app::nollie
