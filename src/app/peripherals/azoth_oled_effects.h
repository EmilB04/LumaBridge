#pragma once

#include <array>
#include <cmath>
#include "azoth_oled.h"

namespace luma::app::azoth {

constexpr std::array<const char*, 6> kAsusAnimationNames{
    "ASUS 1 - ROG diagonal", "ASUS 2 - ROG burst", "ASUS 3 - Fireworks",
    "ASUS 4 - Starfield", "ASUS 5 - Heartbeat", "ASUS 6 - ROG scroll"};
// Names used by Armoury Crate's original-Azoth preset list, in firmware-index order.
constexpr std::array<const wchar_t*, 6> kAsusAnimationFiles{
    L"rog_ani_1", L"rog_ani_6", L"firework", L"rog_ani_4", L"heartbeat", L"rog_ani_8"};
constexpr std::array<const char*, kLumaOledAnimationCount> kLumaAnimationNames{
    "LumaBridge - Wave", "LumaBridge - Level bars", "LumaBridge - Stars",
    "LumaBridge - Scanner", "LumaBridge - Rings", "LumaBridge - Rain",
    "LumaBridge - Spiral", "LumaBridge - Plasma", "LumaBridge - Tunnel",
    "LumaBridge - Bounce", "LumaBridge - Pulse", "LumaBridge - Fireworks",
    "LumaBridge - Orbit", "LumaBridge - Checker", "LumaBridge - Ribbons",
    "LumaBridge - Mountains", "LumaBridge - Comet", "LumaBridge - Dots"};
constexpr std::array<const wchar_t*, kLumaOledAnimationCount> kLumaAnimationFiles{
    L"LumaBridge-Wave.gif", L"LumaBridge-Level-bars.gif", L"LumaBridge-Stars.gif",
    L"LumaBridge-Scanner.gif", L"LumaBridge-Rings.gif", L"LumaBridge-Rain.gif",
    L"LumaBridge-Spiral.gif", L"LumaBridge-Plasma.gif", L"LumaBridge-Tunnel.gif",
    L"LumaBridge-Bounce.gif", L"LumaBridge-Pulse.gif", L"LumaBridge-Fireworks.gif",
    L"LumaBridge-Orbit.gif", L"LumaBridge-Checker.gif", L"LumaBridge-Ribbons.gif",
    L"LumaBridge-Mountains.gif", L"LumaBridge-Comet.gif", L"LumaBridge-Dots.gif"};
constexpr int kOledEffectFrames = 150, kOledEffectDelay = 5; // 20 fps, 7.5-second loop.
constexpr double kOledEffectSeconds = kOledEffectFrames * kOledEffectDelay / 100.0;

// Shared by the preview and GIF export. Every motion completes whole cycles per loop.
inline std::vector<uint8_t> RenderOledEffect(int index, double time) {
    std::vector<uint8_t> pixels(kOledWidth * kOledHeight, 0);
    if (index < 0 || index >= static_cast<int>(kLumaAnimationNames.size()) || !std::isfinite(time)) return pixels;
    double position = std::fmod(time, kOledEffectSeconds);
    if (position < 0) position += kOledEffectSeconds;
    const double phase = position / kOledEffectSeconds;
    const double tau = 6.283185307179586;
    auto rect = [&](double x, double y, double w, double h, double a = 1.0) {
        if (w <= 0 || h <= 0 || a <= 0.02) return;
        const int left = std::clamp(static_cast<int>(std::floor(x)), 0, kOledWidth);
        const int right = std::clamp(static_cast<int>(std::ceil(x + w)), 0, kOledWidth);
        const int top = std::clamp(static_cast<int>(std::floor(y)), 0, kOledHeight);
        const int bottom = std::clamp(static_cast<int>(std::ceil(y + h)), 0, kOledHeight);
        const auto level = static_cast<uint8_t>(std::clamp(a, 0.0, 1.0) * 255 + 0.5);
        for (int yy = top; yy < bottom; ++yy)
            for (int xx = left; xx < right; ++xx) {
                auto& p = pixels[yy * kOledWidth + xx];
                p = std::max(p, level);
            }
    };
    switch (index) {
    case 0:
        for (int x = 0; x < kOledWidth; x += 2)
            rect(x, kOledHeight / 2 + std::sin(x * 0.045 + phase * tau * 3) * 18 - 1, 2, 3);
        break;
    case 1:
        for (int i = 0; i < 24; ++i) {
            const double h = 6 + (0.5 + 0.5 * std::sin(phase * tau * (2 + i % 5) + i * 1.3)) * 50;
            rect(6 + i * 10.4, kOledHeight - 4 - h, 7, h);
        }
        break;
    case 2:
        for (int i = 0; i < 46; ++i) {
            const int speed = 1 + i % 3;
            const double x = std::fmod(i * 97 % kOledWidth - phase * speed * kOledWidth + kOledWidth * 4, kOledWidth);
            rect(x, i * 53 % (kOledHeight - 2), speed + 1, 1, 0.25 + 0.25 * speed);
        }
        break;
    case 3: {
        const double u = phase * 2;
        const double x = (u < 1 ? u : 2 - u) * (kOledWidth - 24);
        for (int k = 0; k < 10; ++k) rect(x - k * 6 * (u < 1 ? 1 : -1), 22, 24, 20, 1 - k * 0.1);
        break;
    }
    case 4:
        for (int k = 0; k < 4; ++k) {
            const double r = std::fmod(phase * 88 * 2 + k * 22, 88);
            const double a = 1 - r / 88, w = r * 2.9, h = std::min(r * 0.72, 30.0);
            const double x = kOledWidth / 2 - w / 2, y = kOledHeight / 2 - h;
            rect(x, y, w, 2, a); rect(x, kOledHeight / 2 + h - 2, w, 2, a);
            rect(x, y, 2, h * 2, a); rect(x + w - 2, y, 2, h * 2, a);
        }
        break;
    case 5:
        for (int col = 0; col < 32; ++col) {
            const double head = std::fmod(phase * (1 + col % 3) * (kOledHeight + 30) + col * 41, kOledHeight + 30);
            for (int k = 0; k < 7; ++k) rect(col * 8 + 2, head - k * 4, 3, 3, 1 - k * 0.14);
        }
        break;
    case 6:
        for (int i = 0; i < 420; ++i) {
            const double r = i / 420.0, a = r * tau * 4 + phase * tau * 2;
            rect(128 + std::cos(a) * r * 122, 32 + std::sin(a) * r * 28, 2, 2, 1 - r * .7);
        }
        break;
    case 7:
        for (int y = 0; y < kOledHeight; y += 2)
            for (int x = 0; x < kOledWidth; x += 2) {
                const double v = (std::sin(x * .05 + phase * tau) + std::sin(y * .15 - phase * tau * 2) +
                                  std::sin((x + y) * .04 + phase * tau * 3)) / 3;
                rect(x, y, 2, 2, .08 + std::pow(.5 + .5 * v, 3) * .92);
            }
        break;
    case 8:
        for (int i = 0; i < 12; ++i) {
            const double u = std::fmod(phase * 2 + i / 12.0, 1.0), w = 250 * u * u, h = 60 * u * u;
            rect(128 - w / 2, 32 - h / 2, w, 1, u);
            rect(128 - w / 2, 32 + h / 2, w, 1, u);
            rect(128 - w / 2, 32 - h / 2, 1, h, u);
            rect(128 + w / 2, 32 - h / 2, 1, h, u);
        }
        break;
    case 9:
        for (int i = 10; i >= 0; --i) {
            const double t = phase * tau - i * .065;
            rect(121 + std::sin(t * 3) * 111, 25 + std::cos(t * 4) * 23, 13, 13, 1 - i * .085);
        }
        break;
    case 10:
        for (int x = 0; x < kOledWidth; ++x) {
            const double u = std::fmod(x / 256.0 + phase * 3, 1.0);
            const double y = 32 + std::exp(-std::pow((u - .5) * 28, 2)) * std::sin(u * tau * 7) * 27;
            rect(x, y, 1, 2);
        }
        break;
    case 11:
        for (int burst = 0; burst < 4; ++burst) {
            const double u = std::fmod(phase * 3 + burst * .27, 1.0);
            for (int ray = 0; ray < 24; ++ray) {
                const double a = ray * tau / 24;
                rect(32 + burst * 63 + std::cos(a) * u * 28, 30 + std::sin(a) * u * 25, 2, 2, 1 - u);
            }
        }
        break;
    case 12:
        for (int ring = 0; ring < 3; ++ring)
            for (int i = 0; i < 90; ++i) {
                const double a = i * tau / 90 + phase * tau * (ring + 1);
                rect(128 + std::cos(a) * (40 + ring * 37), 32 + std::sin(a) * (9 + ring * 9), 2, 2,
                     .15 + .85 * (90 - i) / 90.0);
            }
        break;
    case 13:
        for (int y = -16; y < 80; y += 16)
            for (int x = -16; x < 272; x += 16) {
                const double a = .2 + .8 * (.5 + .5 * std::sin((x - y) * .035 + phase * tau * 3));
                rect(x + phase * 16, y + phase * 16, 12, 12, a);
            }
        break;
    case 14:
        for (int layer = 0; layer < 4; ++layer)
            for (int x = 0; x < 256; x += 2)
                rect(x, 30 + std::sin(x * .035 + phase * tau * 2 + layer * .5) * 22, 2, 2, 1 - layer * .22);
        break;
    case 15:
        for (int layer = 0; layer < 3; ++layer)
            for (int x = 0; x < 256; ++x) {
                const double a = x * tau / 256 + phase * tau * (layer + 1);
                const double h = 9 + (std::sin(a * 3 + layer) + std::sin(a * 5)) * 5;
                rect(x, 64 - layer * 13 - h, 1, h, .25 + layer * .32);
            }
        break;
    case 16:
        for (int i = 0; i < 50; ++i) {
            const double u = phase - i / 500.0;
            rect(128 + std::cos(u * tau * 3) * 117, 32 + std::sin(u * tau * 3) * 25, 3, 3, 1 - i / 50.0);
        }
        break;
    case 17:
        for (int i = 0; i < 20; ++i) {
            const double a = std::pow(.5 + .5 * std::cos(phase * tau * 3 - i * .4), 5);
            rect(10 + i * 12, 25, 7, 14, .08 + a * .92);
        }
        break;
    }
    return pixels;
}

// A GIF89a grayscale animation. Frequent clear codes keep LZW codes at nine bits,
// avoiding a dictionary while producing files accepted by standard GIF decoders.
inline std::vector<uint8_t> OledEffectGif(int index) {
    if (index < 0 || index >= static_cast<int>(kLumaAnimationNames.size())) return {};
    std::vector<uint8_t> out{'G', 'I', 'F', '8', '9', 'a'};
    auto word = [&](int value) { out.push_back(static_cast<uint8_t>(value)); out.push_back(static_cast<uint8_t>(value >> 8)); };
    word(kOledWidth); word(kOledHeight);
    out.insert(out.end(), {0xF7, 0, 0});
    for (int i = 0; i < 256; ++i) out.insert(out.end(), 3, static_cast<uint8_t>(i));
    const uint8_t loop[] = {0x21, 0xFF, 11, 'N','E','T','S','C','A','P','E','2','.','0', 3, 1, 0, 0, 0};
    out.insert(out.end(), std::begin(loop), std::end(loop));
    for (int frame = 0; frame < kOledEffectFrames; ++frame) {
        out.insert(out.end(), {0x21, 0xF9, 4, 4}); // Full opaque frame, keep until the next frame.
        word(kOledEffectDelay); out.insert(out.end(), {0, 0, 0x2C});
        word(0); word(0); word(kOledWidth); word(kOledHeight);
        out.insert(out.end(), {0, 8});
        std::vector<uint8_t> compressed;
        uint32_t bits = 0; int count = 0;
        auto code = [&](int value) {
            bits |= static_cast<uint32_t>(value) << count; count += 9;
            while (count >= 8) { compressed.push_back(static_cast<uint8_t>(bits)); bits >>= 8; count -= 8; }
        };
        const auto pixels = RenderOledEffect(index, frame * kOledEffectDelay / 100.0);
        for (size_t at = 0; at < pixels.size(); ++at) {
            if (at % 200 == 0) code(256);
            code(pixels[at]);
        }
        code(257);
        if (count) compressed.push_back(static_cast<uint8_t>(bits));
        for (size_t at = 0; at < compressed.size(); at += 255) {
            const size_t n = std::min<size_t>(255, compressed.size() - at);
            out.push_back(static_cast<uint8_t>(n));
            out.insert(out.end(), compressed.begin() + at, compressed.begin() + at + n);
        }
        out.push_back(0);
    }
    out.push_back(0x3B);
    return out;
}

} // namespace luma::app::azoth
