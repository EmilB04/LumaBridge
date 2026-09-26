// Turns Chroma effect parameters into an ambient color, and picks which device class
// drives the Aura mirror. Pure logic, unit tested in tests/test_core.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "chroma_types.h"
#include "color.h"

namespace luma::chroma {

inline const char* DeviceClassName(DeviceClass c) {
    switch (c) {
    case DeviceClass::Keyboard: return "keyboard";
    case DeviceClass::Mouse: return "mouse";
    case DeviceClass::Headset: return "headset";
    case DeviceClass::Mousepad: return "mousepad";
    case DeviceClass::Keypad: return "keypad";
    case DeviceClass::ChromaLink: return "chromalink";
    default: return "?";
    }
}

// Returns the ambient color for an effect, or nullopt for effect types we don't emulate
// (deprecated hardware effects such as wave / spectrum cycling). A null `param` on an effect
// that needs one yields nullopt.
inline std::optional<Rgb> TranslateEffect(DeviceClass dc, int type, const void* param,
                                          BitmapReduce mode) {
    auto refs = static_cast<const uint32_t*>(param);
    auto grid = [&](size_t n) -> std::optional<Rgb> {
        if (!refs) return std::nullopt;
        return ReduceColorRefs(refs, n, mode);
    };
    auto single = [&](size_t offsetWords) -> std::optional<Rgb> {
        if (!refs) return std::nullopt;
        return FromColorRef(refs[offsetWords]);
    };

    switch (dc) {
    case DeviceClass::Keyboard:
        switch (type) {
        case keyboard::NONE: return Rgb{};
        case keyboard::STATIC: return single(0);
        case keyboard::CUSTOM:
        case keyboard::CUSTOM_KEY:  // Color grid first; the Key grid overlays it
            return grid(keyboard::kMaxRow * keyboard::kMaxColumn);
        case keyboard::CUSTOM2: return grid(keyboard::kMaxRowExt * keyboard::kMaxColumnExt);
        default: return std::nullopt;
        }
    case DeviceClass::Mouse:
        switch (type) {
        case mouse::NONE: return Rgb{};
        case mouse::STATIC: return single(1);  // { RZLED LEDId; COLORREF Color; }
        case mouse::CUSTOM: return grid(mouse::kMaxLeds);
        case mouse::CUSTOM2: return grid(mouse::kMaxRow * mouse::kMaxColumn);
        default: return std::nullopt;
        }
    case DeviceClass::Headset:
        switch (type) {
        case headset::NONE: return Rgb{};
        case headset::STATIC: return single(0);
        case headset::CUSTOM: return grid(headset::kMaxLeds);
        default: return std::nullopt;
        }
    case DeviceClass::Mousepad:
        switch (type) {
        case mousepad::NONE: return Rgb{};
        case mousepad::STATIC: return single(0);
        case mousepad::CUSTOM: return grid(mousepad::kMaxLeds);
        case mousepad::CUSTOM2: return grid(mousepad::kMaxLeds2);
        default: return std::nullopt;
        }
    case DeviceClass::Keypad:
        switch (type) {
        case keypad::NONE: return Rgb{};
        case keypad::STATIC: return single(0);
        case keypad::CUSTOM: return grid(keypad::kMaxRow * keypad::kMaxColumn);
        default: return std::nullopt;
        }
    case DeviceClass::ChromaLink:
        switch (type) {
        case chromalink::NONE: return Rgb{};
        case chromalink::STATIC: return single(0);
        case chromalink::CUSTOM: return grid(chromalink::kMaxLeds);
        default: return std::nullopt;
        }
    default: return std::nullopt;
    }
}

// Generic CreateEffect(deviceId, ...) -- only the device-independent types.
inline std::optional<Rgb> TranslateGenericEffect(int type, const void* param) {
    switch (type) {
    case generic::NONE: return Rgb{};
    case generic::STATIC:  // { DWORD Size; DWORD Param; COLORREF Color; }
        if (!param) return std::nullopt;
        return FromColorRef(static_cast<const uint32_t*>(param)[2]);
    default: return std::nullopt;
    }
}

// Which device class's color becomes the ambient color.
enum class AmbientSource { Auto, Keyboard, Mouse, Headset, Mousepad, Keypad, ChromaLink };

class AmbientSelector {
public:
    explicit AmbientSelector(AmbientSource src = AmbientSource::Auto) : src_(src) {}

    // Records a device class's new color. Returns the new ambient color if it changed
    // what the mirror should show, nullopt otherwise.
    std::optional<Rgb> Update(DeviceClass dc, Rgb c) {
        auto i = static_cast<size_t>(dc);
        colors_[i] = c;
        seen_[i] = true;
        if (Driver() != dc) return std::nullopt;
        return c;
    }

    // Auto: the keyboard grid is what games put the most effort into, then Chroma Link
    // (designed for "ambient" third-party lighting), then the rest.
    DeviceClass Driver() const {
        switch (src_) {
        case AmbientSource::Keyboard: return DeviceClass::Keyboard;
        case AmbientSource::Mouse: return DeviceClass::Mouse;
        case AmbientSource::Headset: return DeviceClass::Headset;
        case AmbientSource::Mousepad: return DeviceClass::Mousepad;
        case AmbientSource::Keypad: return DeviceClass::Keypad;
        case AmbientSource::ChromaLink: return DeviceClass::ChromaLink;
        case AmbientSource::Auto:
        default:
            for (DeviceClass dc : {DeviceClass::Keyboard, DeviceClass::ChromaLink,
                                   DeviceClass::Headset, DeviceClass::Mousepad,
                                   DeviceClass::Keypad, DeviceClass::Mouse})
                if (seen_[static_cast<size_t>(dc)]) return dc;
            return DeviceClass::Keyboard;
        }
    }

private:
    AmbientSource src_;
    std::array<Rgb, static_cast<size_t>(DeviceClass::Count)> colors_{};
    std::array<bool, static_cast<size_t>(DeviceClass::Count)> seen_{};
};

}  // namespace luma::chroma
