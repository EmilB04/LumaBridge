// HID LampArray: the lighting standard of Windows 11's Dynamic Lighting (HID Usage Tables,
// "Lighting And Illumination" page 0x59). A device that implements it in its firmware can be
// lit by any program, with no vendor software: LumaBridge reads its lamps (and where they
// sit), switches off its own effect (AutonomousMode) and sends every lamp's color.
//
// The reports are defined by usage, not by byte layout: the device's report descriptor says
// where each field is, and Windows' HidP_* functions pack them (lamparray_output.cpp). This
// file is the pure part: the usages, color scaling and batching. Tested.
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "color.h"

namespace luma::app::lamparray {

constexpr uint16_t kPage = 0x59;  // Lighting And Illumination
enum Usage : uint16_t {
    kLampArray = 0x01,
    kAttributesReport = 0x02,
    kLampCount = 0x03,
    kBoundingBoxWidth = 0x04,
    kBoundingBoxHeight = 0x05,
    kBoundingBoxDepth = 0x06,
    kLampArrayKind = 0x07,
    kMinUpdateInterval = 0x08,  // microseconds
    kAttributesRequestReport = 0x20,
    kLampId = 0x21,
    kAttributesResponseReport = 0x22,
    kPositionX = 0x23,  // micrometers
    kPositionY = 0x24,
    kPositionZ = 0x25,
    kRedLevelCount = 0x28,
    kGreenLevelCount = 0x29,
    kBlueLevelCount = 0x2A,
    kIntensityLevelCount = 0x2B,
    kMultiUpdateReport = 0x50,
    kRedChannel = 0x51,
    kGreenChannel = 0x52,
    kBlueChannel = 0x53,
    kIntensityChannel = 0x54,
    kUpdateFlags = 0x55,  // bit 0: the last report of this frame
    kRangeUpdateReport = 0x60,
    kLampIdStart = 0x61,
    kLampIdEnd = 0x62,
    kControlReport = 0x70,
    kAutonomousMode = 0x71,  // 1: the device runs its own effect; 0: the host sets the lamps
};
constexpr uint32_t kUpdateComplete = 0x01;

inline const char* KindName(uint32_t kind) {
    switch (kind) {
    case 1: return "Keyboard";
    case 2: return "Mouse";
    case 3: return "Game controller";
    case 4: return "Peripheral";
    case 5: return "Scene";
    case 6: return "Notification";
    case 7: return "Case";
    case 8: return "Wearable";
    case 9: return "Furniture";
    case 10: return "Art";
    default: return "Device";
    }
}

// 0..255 to a channel with `levels` levels (0 .. levels - 1); 0 or 1 level: on / off.
inline uint32_t Level(uint8_t v, uint32_t levels) {
    if (levels <= 1) return v ? 1 : 0;
    if (levels >= 256) return v;
    return (static_cast<uint32_t>(v) * (levels - 1) + 127) / 255;
}

// Where a lamp sits for an effect: 0 .. columns - 1 by its X position (left to right).
inline int Column(uint32_t x, uint32_t minX, uint32_t maxX, int columns) {
    if (columns <= 1 || maxX <= minX) return 0;
    const double f = static_cast<double>(x - std::min(x, minX)) / (maxX - minX);
    return std::clamp(static_cast<int>(f * (columns - 1) + 0.5), 0, columns - 1);
}

// A frame split into multi-update reports of at most `perReport` lamps: (lamp ID, color).
// The last report of a frame carries kUpdateComplete.
using Batch = std::vector<std::pair<uint16_t, Rgb>>;
inline std::vector<Batch> Batches(const std::vector<std::pair<uint16_t, Rgb>>& lamps, size_t perReport) {
    std::vector<Batch> out;
    if (perReport == 0) return out;
    for (size_t i = 0; i < lamps.size(); i += perReport)
        out.emplace_back(lamps.begin() + static_cast<long>(i),
                         lamps.begin() + static_cast<long>(std::min(lamps.size(), i + perReport)));
    return out;
}

}  // namespace luma::app::lamparray
