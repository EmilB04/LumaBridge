// Platform-independent color math shared by the proxy and the Aura bridge.
// Kept header-only and free of Windows headers so it can be unit tested anywhere.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace luma {

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Rgb& o) const { return !(*this == o); }
    bool IsBlack() const { return r == 0 && g == 0 && b == 0; }
};

inline uint8_t ClampByte(double v) {
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<uint8_t>(v + 0.5);
}

// LogiLed APIs take 0..100 percentages per channel.
inline uint8_t PercentToByte(int percent) {
    percent = std::clamp(percent, 0, 100);
    return static_cast<uint8_t>((percent * 255 + 50) / 100);
}

inline Rgb FromPercent(int r, int g, int b) {
    return Rgb{PercentToByte(r), PercentToByte(g), PercentToByte(b)};
}

inline Rgb Scale(Rgb c, double factor) {
    return Rgb{ClampByte(c.r * factor), ClampByte(c.g * factor), ClampByte(c.b * factor)};
}

// Fully saturated color at `hueDeg` (0 = red, 120 = green, 240 = blue).
inline Rgb FromHue(double hueDeg) {
    double h = std::fmod(hueDeg, 360.0);
    if (h < 0) h += 360.0;
    const double x = 1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0);
    double r = 0, g = 0, b = 0;
    if (h < 60) { r = 1; g = x; }
    else if (h < 120) { r = x; g = 1; }
    else if (h < 180) { g = 1; b = x; }
    else if (h < 240) { g = x; b = 1; }
    else if (h < 300) { r = x; b = 1; }
    else { r = 1; b = x; }
    return Rgb{ClampByte(r * 255), ClampByte(g * 255), ClampByte(b * 255)};
}

// Per-brand calibration: LEDs from different vendors render the same RGB triple differently.
struct ColorCorrection {
    double brightness = 1.0;  // overall multiplier, 0..1 (values > 1 allowed, output is clamped)
    double gainR = 1.0;
    double gainG = 1.0;
    double gainB = 1.0;
    double gamma = 1.0;       // applied to normalised channel before gains; 1.0 = off
};

inline Rgb ApplyCorrection(const ColorCorrection& cc, Rgb c) {
    auto channel = [&](uint8_t v, double gain) {
        double x = v / 255.0;
        if (cc.gamma != 1.0 && cc.gamma > 0.0) x = std::pow(x, cc.gamma);
        return ClampByte(x * gain * cc.brightness * 255.0);
    };
    return Rgb{channel(c.r, cc.gainR), channel(c.g, cc.gainG), channel(c.b, cc.gainB)};
}

// Aura SDK colors are DWORD 0x00BBGGRR.
inline uint32_t ToAuraColor(Rgb c) {
    return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8) |
           (static_cast<uint32_t>(c.b) << 16);
}

inline Rgb FromAuraColor(uint32_t v) {
    return Rgb{static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
               static_cast<uint8_t>((v >> 16) & 0xFF)};
}

// LogiLedSetLightingFromBitmap: 21x6 keys, 4 bytes per key, BGRA order.
constexpr int kLogiBitmapWidth = 21;
constexpr int kLogiBitmapHeight = 6;
constexpr int kLogiBitmapBytesPerKey = 4;
constexpr size_t kLogiBitmapSize = kLogiBitmapWidth * kLogiBitmapHeight * kLogiBitmapBytesPerKey;

enum class BitmapReduce {
    Average,    // mean of all lit (non-black) keys
    Brightest,  // single key with highest luminance
};

inline int Luma(Rgb c) { return 299 * c.r + 587 * c.g + 114 * c.b; }

// Collapse many key/LED colors to a single "ambient" color. `get(i)` returns color i.
// Black entries are ignored for Average so a mostly-dark layout with a few lit keys
// doesn't get averaged down to near-black.
template <typename Getter>
Rgb ReduceColors(size_t count, Getter get, BitmapReduce mode) {
    uint64_t sr = 0, sg = 0, sb = 0, n = 0;
    Rgb best{};
    int bestLuma = -1;
    for (size_t i = 0; i < count; ++i) {
        Rgb c = get(i);
        if (c.IsBlack()) continue;
        sr += c.r;
        sg += c.g;
        sb += c.b;
        ++n;
        int l = Luma(c);
        if (l > bestLuma) {
            bestLuma = l;
            best = c;
        }
    }
    if (n == 0) return Rgb{};
    if (mode == BitmapReduce::Brightest) return best;
    return Rgb{static_cast<uint8_t>((sr + n / 2) / n), static_cast<uint8_t>((sg + n / 2) / n),
               static_cast<uint8_t>((sb + n / 2) / n)};
}

// LogiLed bitmap: BGRA bytes.
inline Rgb ReduceBitmap(const uint8_t* bgra, size_t size, BitmapReduce mode) {
    return ReduceColors(
        size / kLogiBitmapBytesPerKey,
        [bgra](size_t i) {
            const uint8_t* p = bgra + i * kLogiBitmapBytesPerKey;
            return Rgb{p[2], p[1], p[0]};
        },
        mode);
}

// Win32 COLORREF (0x00BBGGRR) arrays, as used by Razer Chroma. The high byte is ignored
// (Chroma uses 0x01000000 to flag "key effect" entries in CUSTOM_KEY grids).
inline Rgb FromColorRef(uint32_t c) { return FromAuraColor(c & 0x00FFFFFFu); }

inline Rgb ReduceColorRefs(const uint32_t* colors, size_t count, BitmapReduce mode) {
    return ReduceColors(count, [colors](size_t i) { return FromColorRef(colors[i]); }, mode);
}

}  // namespace luma
