// F1 2018 to F1 25 lighting from the games' own UDP telemetry: once switched on in the game's
// settings (Telemetry: On, IP 127.0.0.1, port LumaBridge shows), the game sends packets many
// times a second. LumaBridge reads only the Car Telemetry packet (packet ID 6), from EA's public
// UDP specifications. The packet format (the year, the first u16) sets the layout:
//   2018        header 21 bytes, packetId at 3, playerCarIndex at 20, 20 cars of 53 bytes
//               (u16 speed, u8 throttle, i8 steer, u8 brake, u8 clutch, i8 gear, u16 engineRPM,
//               u8 drs, u8 revLightsPercent, ...)
//   2019        header 23 bytes, packetId at 5, playerCarIndex at 22, 20 cars of 66 bytes
//   2020        header 24 bytes, packetId at 5, playerCarIndex at 22, 22 cars of 58 bytes
//   2021, 2022  header 24 bytes, packetId at 5, playerCarIndex at 22, 22 cars of 60 bytes
//   2023 on     header 29 bytes, packetId at 6, playerCarIndex at 27, 22 cars of 60 bytes
// From 2019 on, each car starts the same way:
//   u16 speed, f32 throttle, f32 steer, f32 brake, u8 clutch, i8 gear, u16 engineRPM,
//   u8 drs, u8 revLightsPercent, ...
// revLightsPercent is the game's own shift-light reading (0-100), used directly for rev lights
// instead of deriving one from engine RPM. The games can also send an older year's format
// (their "UDP Format" setting); any of these works. Pure C++, tested; not checked against a live
// packet capture (no PC to run the games on), so treat the offsets as "by the book" until
// confirmed.
#pragma once

#include <cstdint>
#include <cstring>

#include "effects.h"

namespace luma::app::games {

class F1Lighting {
public:
    static constexpr uint16_t kDefaultPort = 20777;
    static constexpr uint64_t kStaleMs = 1000;
    static constexpr size_t kHeaderSize = 29;          // F1 23 on
    static constexpr size_t kCarTelemetrySize = 60;    // F1 2021 on
    static constexpr uint8_t kCarTelemetryPacketId = 6;

    // Where things are in one year's format.
    struct Layout {
        size_t header, packetId, playerIndex, carSize, cars;
        size_t gear = 15, rpm = 16, revLights = 19;  // within a car
    };
    static Layout LayoutFor(uint16_t format) {
        if (format == 2018) return {21, 3, 20, 53, 20, 6, 7, 10};
        if (format == 2019) return {23, 5, 22, 66, 20};
        if (format == 2020) return {24, 5, 22, 58, 22};
        if (format == 2021 || format == 2022) return {24, 5, 22, 60, 22};
        return {kHeaderSize, 6, 27, kCarTelemetrySize, 22};  // 2023, 2024, 2025 and later
    }

    // One UDP packet. False if it isn't a Car Telemetry packet for our own car, or too short.
    bool OnPacket(const uint8_t* p, size_t n, uint64_t now) {
        if (n < 21) return false;
        uint16_t format;
        std::memcpy(&format, p, 2);
        const Layout l = LayoutFor(format);
        if (n < l.header || p[l.packetId] != kCarTelemetryPacketId) return false;
        const uint8_t playerIdx = p[l.playerIndex];
        const size_t carOffset = l.header + static_cast<size_t>(playerIdx) * l.carSize;
        if (playerIdx >= l.cars || n < carOffset + l.carSize) return false;
        uint16_t speed, rpm;
        int8_t gear;
        uint8_t revPercent;
        std::memcpy(&speed, p + carOffset, 2);
        gear = static_cast<int8_t>(p[carOffset + l.gear]);
        std::memcpy(&rpm, p + carOffset + l.rpm, 2);
        revPercent = p[carOffset + l.revLights];
        lastSeen_ = now;
        speedKph_ = speed;
        gear_ = gear;
        // No explicit "on track" flag in this packet: the engine running (RPM > 0) is the best
        // signal a Car Telemetry packet alone gives us.
        running_ = rpm > 0;
        revs_ = revPercent > 100 ? 1.0 : revPercent / 100.0;
        return true;
    }

    // On track with the engine running and sending.
    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && running_; }

    // Rev lights: calm blue at low revs, then green, yellow and red as the shift lights climb,
    // and a fast red flash once they're full (time to shift).
    fx::Params Current() const {
        fx::Params p;
        p.speed = 0;
        if (revs_ >= 1.0) {
            p.kind = fx::Kind::Strobe;
            p.color1 = Rgb{255, 0, 0};
            p.speed = 12;
            return p;
        }
        p.kind = fx::Kind::Static;
        const double r = revs_;
        if (r < 0.5) p.color1 = Rgb{0, 90, 255};
        else if (r < 0.7) p.color1 = Blend(Rgb{0, 90, 255}, Rgb{0, 255, 60}, (r - 0.5) / 0.2);
        else if (r < 0.85) p.color1 = Blend(Rgb{0, 255, 60}, Rgb{255, 220, 0}, (r - 0.7) / 0.15);
        else p.color1 = Blend(Rgb{255, 220, 0}, Rgb{255, 0, 0}, (r - 0.85) / 0.15);
        return p;
    }

    double revs() const { return revs_; }
    double speedKph() const { return speedKph_; }
    int gear() const { return gear_; }

private:
    static Rgb Blend(Rgb a, Rgb b, double t) {
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        return Rgb{static_cast<uint8_t>(a.r + (b.r - a.r) * t), static_cast<uint8_t>(a.g + (b.g - a.g) * t),
                   static_cast<uint8_t>(a.b + (b.b - a.b) * t)};
    }

    uint64_t lastSeen_ = 0;
    bool running_ = false;
    double revs_ = 0;
    double speedKph_ = 0;
    int gear_ = 0;
};

}  // namespace luma::app::games
