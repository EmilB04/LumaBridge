// Original ROG Azoth OLED commands documented by G-Helper's Azoth implementation.
// This is independent of RGB ownership. LumaBridge runs the screen (on/off, brightness); "Keep
// current" leaves what it shows alone, so Armoury Crate's uploads and modes stay.
#pragma once

#include "azoth_protocol.h"

namespace luma::app::azoth {

enum class OledContent { Keep, Animation, Clock };
enum class OledAnimationSource { Asus, LumaBridge };

struct OledSettings {
    bool direct = true;  // always: the Armoury Crate / LumaBridge choice was removed
    bool enabled = true;
    int brightness = 50;
    OledContent content = OledContent::Keep;
    int animation = 0;
    bool clock12Hour = false;
    OledAnimationSource animationSource = OledAnimationSource::Asus;
    int lumaAnimation = 0;
};

inline OledSettings NormalizeOled(OledSettings s) {
    s.brightness = std::clamp(s.brightness, 0, 100);
    s.animation = std::clamp(s.animation, 0, 5);
    s.lumaAnimation = std::clamp(s.lumaAnimation, 0, 5);
    if (s.animationSource != OledAnimationSource::Asus && s.animationSource != OledAnimationSource::LumaBridge)
        s.animationSource = OledAnimationSource::Asus;
    if (s.content != OledContent::Keep && s.content != OledContent::Animation && s.content != OledContent::Clock)
        s.content = OledContent::Keep;
    return s;
}

inline bool operator==(const OledSettings& a, const OledSettings& b) {
    return a.direct == b.direct && a.enabled == b.enabled && a.brightness == b.brightness &&
           a.content == b.content && a.animation == b.animation && a.clock12Hour == b.clock12Hour &&
           a.animationSource == b.animationSource && a.lumaAnimation == b.lumaAnimation;
}
inline bool operator!=(const OledSettings& a, const OledSettings& b) { return !(a == b); }

struct OledTime {
    uint16_t year = 2000;
    uint8_t month = 1, day = 1, hour = 0, minute = 0;
};

inline Report OledCommand(Link link, uint8_t command, uint8_t value = 0) {
    Report r{};
    r[0] = link == Link::Wired ? 0 : 2;
    r[1] = command;
    r[5] = value;
    return r;
}

inline Report ReadOledAnimation(Link link) { return OledCommand(link, 0x21); }

inline Report OledClock(Link link, OledTime time, bool twelveHour) {
    const uint8_t hour = std::min<uint8_t>(time.hour, 23);
    // Format 0 = 24 hour, 1 = AM, 2 = PM. The protocol uses 0 at midnight even in AM mode.
    Report r = OledCommand(link, 0x63, twelveHour ? (hour < 12 ? 1 : 2) : 0);
    r[6] = static_cast<uint8_t>(time.year);
    r[7] = static_cast<uint8_t>(time.year >> 8);
    r[8] = std::clamp<uint8_t>(time.month, 1, 12);
    r[9] = std::clamp<uint8_t>(time.day, 1, 31);
    r[10] = twelveHour && hour > 12 ? hour - 12 : hour;
    r[11] = std::min<uint8_t>(time.minute, 59);
    return r;
}

inline std::vector<Report> OledCommands(OledSettings settings, Link link, OledTime time) {
    const auto s = NormalizeOled(settings);
    if (!s.direct) return {};
    std::vector<Report> out{OledCommand(link, 0x69, s.enabled ? 1 : 0)};
    if (!s.enabled) return out;
    out.push_back(OledCommand(link, 0x68, static_cast<uint8_t>(s.brightness)));
    // Custom GIFs use Armoury Crate's importer; never select a firmware preset in their place.
    if (s.content == OledContent::Animation && s.animationSource == OledAnimationSource::Asus)
        out.push_back(OledCommand(link, 0x61, static_cast<uint8_t>(s.animation)));
    else if (s.content == OledContent::Clock)
        out.push_back(OledClock(link, time, s.clock12Hour));
    return out;
}

enum class OledReply { Unrelated, Accepted, Rejected };
inline OledReply ClassifyOledReply(const Report& sent, const Report& reply, size_t size) {
    if (size < 3 || reply[0] != sent[0]) return OledReply::Unrelated;
    if (reply[1] == 0xFF && reply[2] == 0xAA) return OledReply::Rejected;
    if (reply[1] != sent[1] || reply[2] != sent[2]) return OledReply::Unrelated;
    // The animation query's data is byte 5. Other commands acknowledge their command bytes.
    if (sent[1] == 0x21 && size < 6) return OledReply::Unrelated;
    return OledReply::Accepted;
}

// A 256 x 64, 24-bit BMP for Armoury Crate's image importer. Pixels are grayscale,
// top-to-bottom on input; BMP stores rows bottom-to-top and BGR pixels.
constexpr int kOledWidth = 256, kOledHeight = 64;
inline std::vector<uint8_t> OledBitmap(const std::vector<uint8_t>& gray) {
    if (gray.size() != kOledWidth * kOledHeight) return {};
    std::vector<uint8_t> out(54 + kOledWidth * kOledHeight * 3);
    auto put = [&](size_t at, uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) out[at + i] = static_cast<uint8_t>(value >> (i * 8));
    };
    out[0] = 'B'; out[1] = 'M';
    put(2, static_cast<uint32_t>(out.size()), 4);
    put(10, 54, 4); put(14, 40, 4);
    put(18, kOledWidth, 4); put(22, kOledHeight, 4);
    put(26, 1, 2); put(28, 24, 2);
    put(34, kOledWidth * kOledHeight * 3, 4);
    for (int y = 0; y < kOledHeight; ++y)
        for (int x = 0; x < kOledWidth; ++x) {
            const auto value = gray[y * kOledWidth + x];
            const size_t at = 54 + ((kOledHeight - 1 - y) * kOledWidth + x) * 3;
            out[at] = out[at + 1] = out[at + 2] = value;
        }
    return out;
}

}  // namespace luma::app::azoth
