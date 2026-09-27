// The virtual Corsair devices the emulator reports: a full-size US keyboard and a 4-zone
// mouse. Games enumerate these, read LED positions and then address LEDs by id, so the ids
// only have to be self-consistent: they are assigned here, sequentially, and returned through
// CorsairGetLedPositions / CorsairGetLedIdForKeyName.
//
// Games that hard-code Corsair's own CLK_* enum values still work for the ambient mirror
// (every color that is set counts, whatever the id), but would hit a different key than
// intended once per-key output exists. The real enum table is a milestone-4 item.
//
// Pure C++, no Windows headers: unit tested in tests/test_core.cpp.
#pragma once

#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "color.h"
#include "corsair_types.h"

namespace luma::corsair {

struct FakeLed {
    int id;
    std::string name;  // key label ("W", "Space", "F1"), used for CorsairGetLedIdForKeyName
    double top, left, height, width;  // millimetres, like the real SDK
};

struct FakeDevice {
    CorsairDeviceType type;
    const char* model;
    CorsairPhysicalLayout physicalLayout;
    std::vector<FakeLed> leds;
};

namespace detail {

constexpr double kUnit = 19.05;  // one key width in mm

struct KeyDef {
    const char* name;
    double width;      // in key units
    double gapBefore;  // in key units
};

inline void AddRow(std::vector<FakeLed>& leds, int& nextId, int row,
                   std::initializer_list<KeyDef> keys) {
    double top = row * kUnit + (row > 0 ? kUnit / 2 : 0);  // F-row sits apart from the rest
    double x = 0;
    for (const KeyDef& k : keys) {
        x += k.gapBefore * kUnit;
        leds.push_back(FakeLed{nextId++, k.name, top, x, kUnit, k.width * kUnit});
        x += k.width * kUnit;
    }
}

}  // namespace detail

inline std::vector<FakeDevice> BuildFakeDevices() {
    using detail::AddRow;
    std::vector<FakeDevice> devices;
    int id = 1;

    FakeDevice kb{CDT_Keyboard, "K70 RGB (LumaBridge)", CPL_US, {}};
    AddRow(kb.leds, id, 0,
           {{"Escape", 1, 0}, {"F1", 1, 1}, {"F2", 1, 0}, {"F3", 1, 0}, {"F4", 1, 0},
            {"F5", 1, 0.5}, {"F6", 1, 0}, {"F7", 1, 0}, {"F8", 1, 0}, {"F9", 1, 0.5},
            {"F10", 1, 0}, {"F11", 1, 0}, {"F12", 1, 0}, {"PrintScreen", 1, 0.25},
            {"ScrollLock", 1, 0}, {"PauseBreak", 1, 0}});
    AddRow(kb.leds, id, 1,
           {{"`", 1, 0}, {"1", 1, 0}, {"2", 1, 0}, {"3", 1, 0}, {"4", 1, 0}, {"5", 1, 0},
            {"6", 1, 0}, {"7", 1, 0}, {"8", 1, 0}, {"9", 1, 0}, {"0", 1, 0}, {"-", 1, 0},
            {"=", 1, 0}, {"Backspace", 2, 0}, {"Insert", 1, 0.25}, {"Home", 1, 0},
            {"PageUp", 1, 0}, {"NumLock", 1, 0.25}, {"KeypadSlash", 1, 0},
            {"KeypadAsterisk", 1, 0}, {"KeypadMinus", 1, 0}});
    AddRow(kb.leds, id, 2,
           {{"Tab", 1.5, 0}, {"Q", 1, 0}, {"W", 1, 0}, {"E", 1, 0}, {"R", 1, 0}, {"T", 1, 0},
            {"Y", 1, 0}, {"U", 1, 0}, {"I", 1, 0}, {"O", 1, 0}, {"P", 1, 0}, {"[", 1, 0},
            {"]", 1, 0}, {"\\", 1.5, 0}, {"Delete", 1, 0.25}, {"End", 1, 0},
            {"PageDown", 1, 0}, {"Keypad7", 1, 0.25}, {"Keypad8", 1, 0}, {"Keypad9", 1, 0},
            {"KeypadPlus", 1, 0}});
    AddRow(kb.leds, id, 3,
           {{"CapsLock", 1.75, 0}, {"A", 1, 0}, {"S", 1, 0}, {"D", 1, 0}, {"F", 1, 0},
            {"G", 1, 0}, {"H", 1, 0}, {"J", 1, 0}, {"K", 1, 0}, {"L", 1, 0}, {";", 1, 0},
            {"'", 1, 0}, {"Enter", 2.25, 0}, {"Keypad4", 1, 3.5}, {"Keypad5", 1, 0},
            {"Keypad6", 1, 0}});
    AddRow(kb.leds, id, 4,
           {{"LeftShift", 2.25, 0}, {"Z", 1, 0}, {"X", 1, 0}, {"C", 1, 0}, {"V", 1, 0},
            {"B", 1, 0}, {"N", 1, 0}, {"M", 1, 0}, {",", 1, 0}, {".", 1, 0}, {"/", 1, 0},
            {"RightShift", 2.75, 0}, {"UpArrow", 1, 1.25}, {"Keypad1", 1, 1.25},
            {"Keypad2", 1, 0}, {"Keypad3", 1, 0}, {"KeypadEnter", 1, 0}});
    AddRow(kb.leds, id, 5,
           {{"LeftCtrl", 1.25, 0}, {"LeftGui", 1.25, 0}, {"LeftAlt", 1.25, 0},
            {"Space", 6.25, 0}, {"RightAlt", 1.25, 0}, {"Fn", 1.25, 0},
            {"Application", 1.25, 0}, {"RightCtrl", 1.25, 0}, {"LeftArrow", 1, 0.25},
            {"DownArrow", 1, 0}, {"RightArrow", 1, 0}, {"Keypad0", 2, 0.25},
            {"KeypadPeriodAndDelete", 1, 0}});
    devices.push_back(std::move(kb));

    FakeDevice mouse{CDT_Mouse, "Dark Core RGB (LumaBridge)", CPL_Zones4, {}};
    const char* zones[] = {"MouseZone1", "MouseZone2", "MouseZone3", "MouseZone4"};
    for (int i = 0; i < 4; ++i) mouse.leds.push_back(FakeLed{id++, zones[i], i * 20.0, 0, 20, 60});
    devices.push_back(std::move(mouse));

    return devices;
}

// CorsairGetLedIdForKeyName(char): letters (either case), digits and the unshifted
// punctuation on a US layout. 0 (CLI_Invalid) when unknown.
inline int LedIdForKeyName(const std::vector<FakeDevice>& devices, char key) {
    if (key == 0) return 0;
    char label[2] = {static_cast<char>(std::toupper(static_cast<unsigned char>(key))), 0};
    for (const auto& d : devices) {
        if (d.type != CDT_Keyboard) continue;
        for (const auto& l : d.leds)
            if (l.name == label) return l.id;
    }
    return 0;
}

// Current color of every LED the game has touched, across all fake devices.
class LedFrame {
public:
    void Set(int ledId, int r, int g, int b) {
        auto clamp = [](int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
        colors_[ledId] = Rgb{clamp(r), clamp(g), clamp(b)};
    }

    bool Get(int ledId, Rgb* out) const {
        auto it = colors_.find(ledId);
        if (it == colors_.end()) return false;
        *out = it->second;
        return true;
    }

    Rgb Reduce(BitmapReduce mode) const {
        std::vector<Rgb> v;
        v.reserve(colors_.size());
        for (const auto& kv : colors_) v.push_back(kv.second);
        return ReduceColors(v.size(), [&v](size_t i) { return v[i]; }, mode);
    }

private:
    std::unordered_map<int, Rgb> colors_;
};

}  // namespace luma::corsair
