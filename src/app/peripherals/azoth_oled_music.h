#pragma once

#include "azoth_oled.h"
#include <cmath>
#include <complex>
#include <cstring>
#include <cwchar>
#include <string>

namespace luma::app::azoth {

constexpr size_t kSpectrumBands = 32, kSpectrumSamples = 2048;
using Spectrum = std::array<uint8_t, kSpectrumBands>;

// WASAPI's PCM extensible formats are signed, little endian, and left aligned in
// their containers. Scaling by container width also handles 24 bits in 32 bits.
inline float PlaybackSample(const uint8_t* data, unsigned bytes, bool floating) {
    if (!data) return 0;
    if (floating) {
        if (bytes != 4) return 0;
        float value = 0;
        std::memcpy(&value, data, 4);
        return std::isfinite(value) ? value : 0;
    }
    if (bytes < 2 || bytes > 4) return 0;
    uint32_t raw = 0;
    for (unsigned i = 0; i < bytes; ++i) raw |= uint32_t(data[i]) << (8 * i);
    const uint64_t sign = uint64_t(1) << (bytes * 8 - 1);
    const int64_t value = raw & sign ? int64_t(raw) - int64_t(sign * 2) : int64_t(raw);
    return static_cast<float>(value / double(sign));
}

// Logarithmic frequency bands from desktop playback, with a Hann window to reduce
// leakage. Each band holds its loudest bin in dB (0 dB is a full-scale sine); bands
// without sound stay at kSpectrumSilence.
constexpr float kSpectrumSilence = -120;
using SpectrumDb = std::array<float, kSpectrumBands>;
inline SpectrumDb AudioBands(const std::array<float, kSpectrumSamples>& samples, unsigned rate) {
    SpectrumDb bands;
    bands.fill(kSpectrumSilence);
    if (rate < 8000 || rate > 384000) return bands;
    constexpr double tau = 6.283185307179586;
    std::array<std::complex<double>, kSpectrumSamples> fft{};
    for (size_t i = 0; i < samples.size(); ++i) {
        const double sample = std::isfinite(samples[i]) ? std::clamp<double>(samples[i], -1, 1) : 0;
        fft[i] = sample * (.5 - .5 * std::cos(tau * i / (samples.size() - 1)));
    }
    for (size_t i = 1, j = 0; i < fft.size(); ++i) {
        size_t bit = fft.size() >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(fft[i], fft[j]);
    }
    for (size_t length = 2; length <= fft.size(); length <<= 1) {
        const auto step = std::polar(1.0, -tau / length);
        for (size_t i = 0; i < fft.size(); i += length) {
            std::complex<double> w(1, 0);
            for (size_t j = 0; j < length / 2; ++j, w *= step) {
                const auto a = fft[i + j], b = fft[i + j + length / 2] * w;
                fft[i + j] = a + b; fft[i + j + length / 2] = a - b;
            }
        }
    }
    const double high = std::min(16000.0, rate / 2.0);
    for (size_t band = 0; band < bands.size(); ++band) {
        const double lowHz = 40 * std::pow(high / 40, band / double(bands.size()));
        const double highHz = 40 * std::pow(high / 40, (band + 1) / double(bands.size()));
        const size_t first = std::max<size_t>(1, static_cast<size_t>(std::ceil(lowHz * fft.size() / rate)));
        const size_t last = std::min(fft.size() / 2, std::max(first + 1, static_cast<size_t>(std::ceil(highHz * fft.size() / rate))));
        double peak = 0;
        for (size_t bin = first; bin < last; ++bin) peak = std::max(peak, std::abs(fft[bin]) * 4 / fft.size());
        if (peak > 1e-6) bands[band] = static_cast<float>(std::max<double>(kSpectrumSilence, 20 * std::log10(peak)));
    }
    return bands;
}

// Fixed scale: -60 dB to 0 dB maps to the screen's 64-pixel height.
inline Spectrum AudioSpectrum(const std::array<float, kSpectrumSamples>& samples, unsigned rate) {
    Spectrum levels{};
    const auto bands = AudioBands(samples, rate);
    for (size_t i = 0; i < levels.size(); ++i)
        if (bands[i] > -60) levels[i] = static_cast<uint8_t>(std::clamp((bands[i] + 60) / 60 * 64, 0.f, 64.f));
    return levels;
}

// Loopback audio follows the Windows volume, and music rarely gets near full scale,
// so a fixed scale left short bars. This scale follows the loudest recent band: it
// rises at once and falls slowly, so quiet songs fill the screen too. High bands get
// a small lift because music has less energy there. Near-silence stays dark.
class SpectrumScale {
public:
    static constexpr float kRange = 30, kTilt = 9, kFallPerSecond = 4, kQuietest = -50, kGate = -75;
    Spectrum Apply(const SpectrumDb& bands, uint64_t nowMs) {
        SpectrumDb lifted;
        float loudest = kSpectrumSilence;
        for (size_t i = 0; i < bands.size(); ++i) {
            lifted[i] = bands[i] + kTilt * i / (bands.size() - 1);
            if (bands[i] > kGate) loudest = std::max(loudest, lifted[i]);
        }
        const float elapsed = last_ ? std::min(5.f, (nowMs - last_) / 1000.f) : 0;
        last_ = nowMs;
        reference_ = std::max({loudest, kQuietest, reference_ - kFallPerSecond * elapsed});
        Spectrum levels{};
        for (size_t i = 0; i < bands.size(); ++i)
            if (bands[i] > kGate)
                levels[i] = static_cast<uint8_t>(std::clamp((lifted[i] - reference_ + kRange) / kRange * 64, 0.f, 64.f));
        return levels;
    }
    float reference() const { return reference_; }
private:
    float reference_ = kQuietest;
    uint64_t last_ = 0;
};

// Original Azoth (M701), ASUS HAL SetMusicMode / SetMusicMode_MusicInfo /
// SetMusicMode_Spectrum. Live music packets use RAM rather than the custom GIF slot.
inline Report OledMusicMode(bool spectrum, size_t bytes = 0) {
    Report r = OledCommand(Link::Wired, 0x67);
    r[5] = spectrum ? 1 : 0;
    r[6] = spectrum ? 1 : 0; // Armoury Crate's default spectrum / song-only styles.
    const size_t chunks = (bytes + 59) / 60;
    r[7] = static_cast<uint8_t>(chunks); r[8] = static_cast<uint8_t>(chunks >> 8);
    r[9] = 208; r[11] = 64;
    return r;
}
inline Report OledSpectrum(const Spectrum& levels) {
    Report r = OledCommand(Link::Wired, 0x67);
    r[2] = 2;
    for (size_t i = 0; i < levels.size(); ++i) r[5 + i] = std::min<uint8_t>(64, levels[i]);
    return r;
}
inline std::vector<uint8_t> OledSongPixels(const std::vector<uint8_t>& gray) {
    if (gray.size() != 208 * 64) return {};
    std::vector<uint8_t> out(gray.size() / 2);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<uint8_t>((gray[i * 2] >> 4) | (gray[i * 2 + 1] & 0xF0));
    return out;
}
inline Report OledSongPart(const std::vector<uint8_t>& data, size_t part) {
    Report r = OledCommand(Link::Wired, 0x67);
    r[2] = 1;
    const size_t chunks = (data.size() + 59) / 60;
    if (part >= chunks) return {};
    const size_t remaining = chunks - part - 1;
    r[3] = static_cast<uint8_t>(remaining); r[4] = static_cast<uint8_t>(remaining >> 8);
    const size_t at = part * 60;
    std::copy_n(data.begin() + at, std::min<size_t>(60, data.size() - at), r.begin() + 5);
    return r;
}

// Players report their position only now and then (on play, pause or seek), so the
// position in between is counted on from the last report while playing. Times are in
// milliseconds; a duration of 0 means the player shares no timeline.
inline int64_t SongPosition(int64_t reportedMs, int64_t durationMs, int64_t sinceReportMs, bool playing) {
    if (durationMs <= 0) return 0;
    const int64_t position = reportedMs + (playing ? std::max<int64_t>(0, sinceReportMs) : 0);
    return std::clamp<int64_t>(position, 0, durationMs);
}
inline std::wstring SongClock(int64_t ms) {
    const int64_t seconds = std::max<int64_t>(0, ms) / 1000;
    wchar_t text[24];
    if (seconds >= 3600)
        std::swprintf(text, std::size(text), L"%lld:%02lld:%02lld", static_cast<long long>(seconds / 3600),
                      static_cast<long long>(seconds / 60 % 60), static_cast<long long>(seconds % 60));
    else
        std::swprintf(text, std::size(text), L"%lld:%02lld", static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
    return text;
}
// Filled width of a progress bar `width` pixels wide.
inline int SongBarFill(int64_t positionMs, int64_t durationMs, int width) {
    if (durationMs <= 0 || width <= 0) return 0;
    return static_cast<int>(std::clamp<int64_t>(positionMs, 0, durationMs) * width / durationMs);
}

struct MusicSnapshot {
    Spectrum levels{};
    std::wstring title, artist;
    int64_t positionMs = 0, durationMs = 0;
    std::string status;
};

} // namespace luma::app::azoth
