// One metadata table for regular lighting, boot lighting and device indicators.
#pragma once
#include "effects.h"
namespace luma::app {
struct EffectInfo {
    const char* name;
    const char* help;
    float minSpeed, maxSpeed;  // maxSpeed 0: no speed slider
    const char* speedFmt;
};

// Indexed by fx::Kind.
inline constexpr EffectInfo kEffects[] = {
    {"Static", "One color on every LED.", 0, 0, ""},
    {"Breathing", "Fades your color in and out.", 0.1f, 3.f, "%.1f breaths per second"},
    {"Strobe", "Flashes your color on and off.", 0.1f, 10.f, "%.1f flashes per second"},
    {"Color cycle", "Every LED shows the same color, cycling through the spectrum.", 0, 0, ""},
    {"Rainbow wave", "The whole spectrum around each fan, turning.", 0.05f, 2.f, "%.2f turns per second"},
    {"Gradient", "Blends your two colors around each fan. Speed 0 keeps it still.", 0.f, 2.f,
     "%.2f turns per second"},
    {"Comet", "A comet of your color with a fading tail chases around, over the second color.", 0.1f, 3.f,
     "%.1f laps per second"},
    {"Twinkle", "Your color with sparkles of the second color.", 0.1f, 3.f, "%.1f per second"},
    {"Beat", "Flashes that speed up (a game's bomb).", 0, 0, ""},  // games only: not in the picker
    // Setup effects (Lighting > Setup effects), across every part of the setup.
    {"Fill one by one", "Each fan, strip, stick and device fills up in turn until everything is lit; then dark, and again.",
     0.2f, 8.f, "%.1f parts per second"},
    {"One at a time", "One part lights up and fades, then the next, through the whole setup.", 0.2f, 8.f,
     "%.1f parts per second"},
    {"Bounce", "A comet runs through every part to the last one and back.", 0.2f, 8.f, "%.1f parts per second"},
    {"Rainbow by part", "Every part its own color of the rainbow, the colors moving along the setup.", 0.2f, 8.f,
     "%.1f parts per second"},
    {"Lightning", "Parts flash at random, like a thunderstorm.", 0.2f, 8.f, "%.1f flashes per second"},
    {"Alternate", "Every other part in the first color, the rest in the second, swapping.", 0.2f, 8.f,
     "%.1f swaps per second"},
    {"Ripple", "A pulse starts in the middle of the setup and runs out to both ends.", 0.2f, 8.f, "%.1f per second"},
    {"Countdown", "Everything lit, then one part after another goes dark; then all again.", 0.2f, 8.f,
     "%.1f parts per second"},
    {"Fill to solid", "Each part fills in turn, then holds the main color.", 0.2f, 8.f, "%.1f parts per second"},
    {"Fill to pulse", "Each part fills in turn, then everything pulses gently together.", 0.2f, 8.f, "%.1f parts per second"},
    {"Center to solid", "Light spreads from the center to both ends, then holds.", 0.2f, 8.f, "%.1f parts per second"},
    {"Center to pulse", "Light spreads from the center, pulses, then retreats to the center and repeats. Boot playback settles into a pulse.", 0.2f, 8.f, "%.1f parts per second"},
    {"Ignite to solid", "Each whole part fades up in turn, then holds.", 0.2f, 8.f, "%.1f parts per second"},
    {"Ignite to pulse", "Each whole part fades up in turn, then everything pulses gently together.", 0.2f, 8.f, "%.1f parts per second"},
};
static_assert(sizeof(kEffects) / sizeof(kEffects[0]) == fx::kFirstSetupEffect + fx::kSetupEffects, "one entry per fx::Kind");


inline const EffectInfo& EffectInfoFor(fx::Kind kind) {
    const int index = static_cast<int>(kind);
    return kEffects[index >= 0 && index < static_cast<int>(sizeof(kEffects) / sizeof(kEffects[0])) ? index : 0];
}
}  // namespace luma::app
