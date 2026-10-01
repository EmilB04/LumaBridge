// DiRT Rally / DiRT Rally 2.0 lighting from the games' own UDP telemetry. It's switched on in
// a file, Documents\My Games\DiRT Rally 2.0\hardwaresettings\hardware_settings_config.xml:
//   <udp enabled="true" extradata="3" ip="127.0.0.1" port="20777" delay="1" />
// With extradata 3 the game sends 64+ little-endian floats per packet; LumaBridge reads
//   byte 132 gear, 148 engine rate, 252 max rpm, 256 idle rpm
// (engine rate and max rpm use the same scale, so their ratio is the rev reading). Pure C++,
// tested; the offsets follow the layout the community documents for the format and aren't
// checked against the running game.
#pragma once

#include <cstdint>
#include <cstring>

#include "effects.h"
#include "rev_lights.h"

namespace luma::app::games {

class DirtLighting {
public:
    static constexpr uint16_t kDefaultPort = 20777;
    static constexpr uint64_t kStaleMs = 1000;
    static constexpr size_t kMinSize = 260;

    bool OnPacket(const uint8_t* p, size_t n, uint64_t now) {
        if (n < kMinSize) return false;
        float rate, maxRpm, idle;
        std::memcpy(&rate, p + 148, 4);
        std::memcpy(&maxRpm, p + 252, 4);
        std::memcpy(&idle, p + 256, 4);
        if (!(maxRpm > 0)) return false;
        lastSeen_ = now;
        running_ = rate > 0;
        const double span = maxRpm - idle;
        revs_ = span > 0 ? Clamp01((rate - idle) / span) : 0;
        return true;
    }

    // Driving a stage (the menus send zeros) and sending.
    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && running_; }

    fx::Params Current() const { return RevLights(revs_, false); }

private:
    uint64_t lastSeen_ = 0;
    bool running_ = false;
    double revs_ = 0;
};

}  // namespace luma::app::games
