// iRacing lighting from the iRacing SDK: while the sim runs it publishes its telemetry in the
// memory block "Local\IRSDKMemMapFileName" for telemetry apps (iRacing's own documented SDK,
// irsdk_defines.h). LumaBridge opens it read-only by name; nothing touches the sim. The block:
//   header: i32 ver (0), i32 status (4, bit 0 connected), ... i32 numVars (24),
//           i32 varHeaderOffset (28), i32 numBuf (32), i32 bufLen (36), pad,
//           varBuf[4] at 48, 16 bytes each: i32 tickCount, i32 bufOffset, pad
//   var headers (144 bytes each): i32 type, i32 offset, i32 count, bool, pad[3], char name[32], ...
// Values are read by name from the newest buffer (the highest tickCount):
//   IsOnTrack (bool), RPM (float), PlayerCarSLFirstRPM / SLShiftRPM / SLBlinkRPM (float, the
//   car's own shift lights), EngineWarnings (bit 0x10 pit limiter, 0x20 rev limiter),
//   SessionFlags (0x8 yellow, 0x20 blue, 0x100 yellow waving, 0x4000 caution,
//   0x8000 caution waving, 0x10000 black).
// Pure C++, tested with a block built the same way; not checked against the running sim.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "effects.h"
#include "rev_lights.h"

namespace luma::app::games {

class IRacingLighting {
public:
    static constexpr uint64_t kStaleMs = 1000;
    static constexpr size_t kHeaderSize = 112, kVarHeaderSize = 144;
    static constexpr uint32_t kPitLimiter = 0x10, kRevLimiter = 0x20;
    static constexpr uint32_t kYellow = 0x8 | 0x100 | 0x4000 | 0x8000, kBlue = 0x20, kBlack = 0x10000;
    enum Type { kChar = 0, kBool = 1, kInt = 2, kBitField = 3, kFloat = 4, kDouble = 5 };

    // One look at the whole block. False if the sim isn't connected or the block doesn't hold
    // the values.
    bool OnBlock(const uint8_t* p, size_t n, uint64_t now) {
        if (!p || n < kHeaderSize) return false;
        const int32_t status = I32(p, 4), numVars = I32(p, 24), varOffset = I32(p, 28), numBuf = I32(p, 32);
        if (!(status & 1) || numVars <= 0 || numVars > 10000 || varOffset < 0 || numBuf < 1 || numBuf > 4 ||
            static_cast<size_t>(varOffset) + static_cast<size_t>(numVars) * kVarHeaderSize > n) {
            onTrack_ = false;
            return false;
        }
        // The newest buffer.
        int32_t tick = -1, buf = -1;
        for (int i = 0; i < numBuf; ++i) {
            const int32_t t = I32(p, 48 + 16 * i);
            if (t > tick) {
                tick = t;
                buf = I32(p, 48 + 16 * i + 4);
            }
        }
        if (buf < 0 || static_cast<size_t>(buf) >= n) return false;
        Var onTrack, rpm, first, shift, blink, warnings, flags;
        for (int32_t i = 0; i < numVars; ++i) {
            const uint8_t* h = p + varOffset + static_cast<size_t>(i) * kVarHeaderSize;
            char name[33] = {};
            std::memcpy(name, h + 16, 32);
            const Var v{I32(h, 0), I32(h, 4)};
            if (!std::strcmp(name, "IsOnTrack")) onTrack = v;
            else if (!std::strcmp(name, "RPM")) rpm = v;
            else if (!std::strcmp(name, "PlayerCarSLFirstRPM")) first = v;
            else if (!std::strcmp(name, "PlayerCarSLShiftRPM")) shift = v;
            else if (!std::strcmp(name, "PlayerCarSLBlinkRPM")) blink = v;
            else if (!std::strcmp(name, "EngineWarnings")) warnings = v;
            else if (!std::strcmp(name, "SessionFlags")) flags = v;
        }
        if (onTrack.offset < 0 || rpm.offset < 0) return false;
        const uint8_t* data = p + buf;
        const size_t room = n - static_cast<size_t>(buf);
        onTrack_ = Read(data, room, onTrack) != 0;
        if (tick != lastTick_ || !lastSeen_) {
            lastTick_ = tick;
            lastSeen_ = now;  // the sim is still writing
        }
        rpm_ = Read(data, room, rpm);
        if (rpm_ > peak_) peak_ = rpm_;
        first_ = Read(data, room, first);
        shift_ = Read(data, room, shift);
        blink_ = Read(data, room, blink);
        warnings_ = static_cast<uint32_t>(Read(data, room, warnings));
        flags_ = static_cast<uint32_t>(Read(data, room, flags));
        return true;
    }

    // In the car, on track, and the sim still writing.
    bool Active(uint64_t now) const { return onTrack_ && lastSeen_ && now - lastSeen_ < kStaleMs; }

    fx::Params Current() const {
        fx::Params p;
        if (flags_ & kBlack) {
            p.kind = fx::Kind::Strobe;
            p.color1 = Rgb{255, 255, 255};
            p.speed = 2;
            return p;
        }
        if (flags_ & kYellow) {
            p.kind = fx::Kind::Strobe;
            p.color1 = Rgb{255, 200, 0};
            p.speed = 2;
            return p;
        }
        if (flags_ & kBlue) {
            p.kind = fx::Kind::Breathing;
            p.color1 = Rgb{0, 80, 255};
            p.speed = 2;
            return p;
        }
        if (warnings_ & kPitLimiter) {
            p.kind = fx::Kind::Breathing;
            p.color1 = Rgb{255, 170, 0};
            p.speed = 1.5;
            return p;
        }
        const bool limit = (warnings_ & kRevLimiter) || (blink_ > 0 && rpm_ >= blink_);
        // From the car's first shift light to its shift point, as 0.5 to 0.95 of the rev lights
        // (blue below the first light); without them, the highest engine speed seen.
        double revs;
        if (shift_ > first_ && first_ > 0) {
            revs = rpm_ < first_ ? 0.5 * rpm_ / first_ : 0.5 + 0.45 * (rpm_ - first_) / (shift_ - first_);
        } else {
            revs = rpm_ / (peak_ > 3000 ? peak_ : 3000);
        }
        return RevLights(Clamp01(revs), limit);
    }

    void Reset() { *this = IRacingLighting(); }

private:
    struct Var {
        int32_t type = -1, offset = -1;
    };
    static int32_t I32(const uint8_t* p, size_t at) {
        int32_t v;
        std::memcpy(&v, p + at, 4);
        return v;
    }
    static double Read(const uint8_t* data, size_t room, const Var& v) {
        if (v.offset < 0) return 0;
        const size_t at = static_cast<size_t>(v.offset);
        switch (v.type) {
        case kChar:
        case kBool: return at < room ? data[at] : 0;
        case kInt:
        case kBitField: return at + 4 <= room ? static_cast<double>(static_cast<uint32_t>(I32(data, at))) : 0;
        case kFloat: {
            if (at + 4 > room) return 0;
            float f;
            std::memcpy(&f, data + at, 4);
            return f;
        }
        case kDouble: {
            if (at + 8 > room) return 0;
            double d;
            std::memcpy(&d, data + at, 8);
            return d;
        }
        default: return 0;
        }
    }

    uint64_t lastSeen_ = 0;
    int32_t lastTick_ = -1;
    bool onTrack_ = false;
    double rpm_ = 0, peak_ = 0, first_ = 0, shift_ = 0, blink_ = 0;
    uint32_t warnings_ = 0, flags_ = 0;
};

}  // namespace luma::app::games
