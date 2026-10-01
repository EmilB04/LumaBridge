// X-Plane 11 / 12 lighting from the sim's own UDP data output: in Settings > Data Output, tick
// the "Network via UDP" box for rows 3 (speeds) and 4 (Mach, VVI, G-load), and in the sim's
// network settings send them to IP 127.0.0.1, the port LumaBridge shows (49003 by default).
// A packet is "DATA" and a separator byte, then 36-byte records: an i32 row number and eight
// f32 values.
//   row 3: [0] indicated airspeed (knots)       row 4: [4] G-load (normal)
// Pure C++, tested; the rows are the numbers X-Plane's Data Output screen shows, which can
// differ between versions, and aren't checked against the running sim.
#pragma once

#include <cstdint>
#include <cstring>

#include "effects.h"

namespace luma::app::games {

class XPlaneLighting {
public:
    static constexpr uint16_t kDefaultPort = 49003;
    static constexpr uint64_t kStaleMs = 2000;

    bool OnPacket(const uint8_t* p, size_t n, uint64_t now) {
        if (n < 5 + 36 || std::memcmp(p, "DATA", 4) != 0) return false;
        bool any = false;
        for (size_t off = 5; off + 36 <= n; off += 36) {
            int32_t row;
            float v[8];
            std::memcpy(&row, p + off, 4);
            std::memcpy(v, p + off + 4, 32);
            if (row == 3) {
                speed_ = v[0];
                haveSpeed_ = true;
                any = true;
            } else if (row == 4) {
                g_ = v[4];
                any = true;
            }
        }
        if (any) lastSeen_ = now;
        return any;
    }

    // In the air or rolling (the sim sends zeros while parked or paused), and sending.
    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && haveSpeed_; }

    // Calm blue; amber from 2.5 G, red from 4 G; magenta under negative G; a slow dim blue
    // while standing still.
    fx::Params Current() const {
        fx::Params p;
        p.speed = 0;
        p.kind = fx::Kind::Static;
        if (g_ < -0.3) {
            p.color1 = Rgb{255, 0, 200};
        } else if (g_ >= 4) {
            p.kind = fx::Kind::Strobe;
            p.color1 = Rgb{255, 0, 0};
            p.speed = 6;
        } else if (g_ >= 2.5) {
            const double t = (g_ - 2.5) / 1.5;
            p.color1 = Rgb{255, static_cast<uint8_t>(170 * (1 - t)), 0};
        } else if (speed_ < 5) {
            p.kind = fx::Kind::Breathing;
            p.color1 = Rgb{0, 60, 160};
            p.speed = 0.4;
        } else {
            p.color1 = Rgb{0, 120, 255};
        }
        return p;
    }

    double g() const { return g_; }
    double speedKnots() const { return speed_; }

private:
    uint64_t lastSeen_ = 0;
    bool haveSpeed_ = false;
    double speed_ = 0, g_ = 1;
};

}  // namespace luma::app::games
