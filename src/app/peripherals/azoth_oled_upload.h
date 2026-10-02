// Original Azoth (M701) custom animation format and wired upload reports, recovered
// from ASUS's installed keyboard SDK and HAL. See docs/azoth-oled-research.md.
#pragma once

#include "azoth_oled.h"

namespace luma::app::azoth {

constexpr int kOledUploadWidth = 208, kOledUploadMaxFrames = 196;
constexpr size_t kOledUploadChunk = 60;
struct OledUploadFrame {
    std::vector<uint8_t> gray; // 256 x 64, composited on black, matching the preview.
    unsigned delayMs = 50;
};

inline std::vector<uint8_t> OledUploadData(const std::vector<OledUploadFrame>& frames) {
    if (frames.empty() || frames.size() > kOledUploadMaxFrames) return {};
    for (const auto& frame : frames)
        if (frame.gray.size() != kOledWidth * kOledHeight || frame.delayMs < 40 || frame.delayMs > 65535) return {};
    std::vector<uint8_t> out;
    out.reserve(2 + frames.size() * (2 + kOledUploadWidth * kOledHeight / 2));
    auto word = [&](unsigned value) {
        out.push_back(static_cast<uint8_t>(value));
        out.push_back(static_cast<uint8_t>(value >> 8));
    };
    word(static_cast<unsigned>(frames.size()));
    for (const auto& frame : frames) word(frame.delayMs);
    // The firmware reserves the remaining screen width for its status icons. Fit the
    // whole preview into the 208-pixel artwork area, with two 4-bit pixels per byte.
    for (const auto& frame : frames)
        for (int y = 0; y < kOledHeight; ++y)
            for (int x = 0; x < kOledUploadWidth; x += 2) {
                const auto lo = frame.gray[y * kOledWidth + x * kOledWidth / kOledUploadWidth] >> 4;
                const auto hi = frame.gray[y * kOledWidth + (x + 1) * kOledWidth / kOledUploadWidth] >> 4;
                out.push_back(static_cast<uint8_t>(lo | (hi << 4)));
            }
    return out;
}

inline size_t OledUploadChunks(const std::vector<uint8_t>& data) {
    return (data.size() + kOledUploadChunk - 1) / kOledUploadChunk;
}

inline Report OledUploadBegin(const std::vector<uint8_t>& data) {
    const size_t chunks = OledUploadChunks(data);
    if (!chunks || chunks > 65535) return {};
    Report r = OledCommand(Link::Wired, 0x61);
    r[2] = 1;
    r[5] = static_cast<uint8_t>(chunks);
    r[6] = static_cast<uint8_t>(chunks >> 8);
    return r;
}

inline Report OledUploadPart(const std::vector<uint8_t>& data, size_t index) {
    const size_t chunks = OledUploadChunks(data);
    if (!chunks || chunks > 65535 || index >= chunks) return {};
    Report r = OledCommand(Link::Wired, 0x61);
    r[2] = 2;
    const size_t remaining = chunks - index - 1;
    r[3] = static_cast<uint8_t>(remaining);
    r[4] = static_cast<uint8_t>(remaining >> 8);
    const size_t at = index * kOledUploadChunk;
    std::copy_n(data.begin() + at, std::min(kOledUploadChunk, data.size() - at), r.begin() + 5);
    return r;
}

inline Report OledUploadShow() {
    Report r = OledCommand(Link::Wired, 0x61);
    r[2] = 3;
    return r;
}

} // namespace luma::app::azoth
