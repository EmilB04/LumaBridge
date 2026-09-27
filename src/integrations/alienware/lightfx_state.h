// Alienware LightFX (AlienFX SDK 2.x, LightFX.dll) semantics, re-declared from the public
// SDK documentation, plus the pure state machine behind the emulator.
//
// LightFX buffers every change until LFX_Update(). The emulator exposes one virtual device
// with one light ("Ambient") covering every location, which is exactly the single-color
// model the Aura mirror drives. Unit tested in tests/test_core.cpp.
#pragma once

#include <cstdint>
#include <optional>

#include "color.h"

namespace luma::lightfx {

using LFX_RESULT = unsigned int;
constexpr LFX_RESULT LFX_SUCCESS = 0;
constexpr LFX_RESULT LFX_FAILURE = 1;
constexpr LFX_RESULT LFX_ERROR_NOINIT = 2;
constexpr LFX_RESULT LFX_ERROR_NODEVS = 3;
constexpr LFX_RESULT LFX_ERROR_NOLIGHTS = 4;
constexpr LFX_RESULT LFX_ERROR_BUFFSIZE = 5;

constexpr unsigned char LFX_DEVTYPE_DESKTOP = 0x02;

constexpr unsigned int LFX_ACTION_MORPH = 0x00000001;
constexpr unsigned int LFX_ACTION_PULSE = 0x00000002;
constexpr unsigned int LFX_ACTION_COLOR = 0x00000003;

constexpr int kDefaultTimingMs = 200;

struct LFX_COLOR {
    unsigned char red;
    unsigned char green;
    unsigned char blue;
    unsigned char brightness;
};

struct LFX_POSITION {
    unsigned char x;
    unsigned char y;
    unsigned char z;
};

// Packed form used by LFX_Light / LFX_ActionColor: 0xBBRRGGBB, top byte = brightness
// (LFX_FULL_BRIGHTNESS = 0xFF000000; 0 brightness = off).
inline Rgb FromPacked(unsigned int v) {
    const double k = ((v >> 24) & 0xFF) / 255.0;
    return Scale(Rgb{static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
                     static_cast<uint8_t>(v & 0xFF)},
                 k);
}

inline Rgb FromStruct(const LFX_COLOR& c) {
    return Scale(Rgb{c.red, c.green, c.blue}, c.brightness / 255.0);
}

struct Command {
    enum class Kind { Static, Pulse } kind = Kind::Static;
    Rgb color{};
    int periodMs = kDefaultTimingMs;
};

class State {
public:
    void Reset() { Stage(Command{Command::Kind::Static, Rgb{}, timingMs_}); }

    void SetTiming(int ms) { timingMs_ = ms > 0 ? ms : kDefaultTimingMs; }

    void SetColor(Rgb c) { Stage(Command{Command::Kind::Static, c, timingMs_}); }

    // MORPH fades from primary to secondary (or from the current color to primary when there
    // is no secondary); the emulator jumps straight to the end color. PULSE blinks primary.
    void Action(unsigned int action, Rgb primary, std::optional<Rgb> secondary) {
        switch (action) {
        case LFX_ACTION_PULSE:
            Stage(Command{Command::Kind::Pulse, primary, timingMs_ * 2});
            break;
        case LFX_ACTION_MORPH:
            SetColor(secondary ? *secondary : primary);
            break;
        case LFX_ACTION_COLOR:
        default:
            SetColor(primary);
            break;
        }
    }

    // LFX_Update: returns the staged command if anything changed since the last update.
    std::optional<Command> Update() {
        if (!dirty_) return std::nullopt;
        dirty_ = false;
        current_ = staged_;
        return current_;
    }

    // What LFX_GetLightColor reports: the last *applied* color.
    Rgb CurrentColor() const { return current_.color; }

private:
    void Stage(Command c) {
        staged_ = c;
        dirty_ = true;
    }

    Command staged_{};
    Command current_{};
    bool dirty_ = false;
    int timingMs_ = kDefaultTimingMs;
};

}  // namespace luma::lightfx
