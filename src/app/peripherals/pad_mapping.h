// Button mapping for game controllers: each button can press a keyboard key or a mouse button,
// and a stick can move the mouse. Pure C++, tested; the keys and clicks are sent by pad_input.cpp.
// A mapping never hides the controller from games, and there is no virtual gamepad here (that
// needs a driver); it only adds keyboard and mouse input.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "pad_protocol.h"

namespace luma::app::pad {

enum class Action { None, Key, Mouse };

// What a button does. Key: `code` is a Windows virtual-key code. Mouse: 1 left, 2 right, 3 middle,
// 4 wheel up, 5 wheel down (one notch a press).
struct Binding {
    Action action = Action::None;
    uint16_t code = 0;
    bool operator==(const Binding& o) const { return action == o.action && code == o.code; }
    bool operator!=(const Binding& o) const { return !(*this == o); }
};

struct KeyOption {
    uint16_t vk;
    const char* name;
};

inline const std::vector<KeyOption>& Keys() {
    static const std::vector<KeyOption> keys = [] {
        std::vector<KeyOption> k;
        static const char* letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        static char letterNames[26][2];
        for (int i = 0; i < 26; ++i) {
            letterNames[i][0] = letters[i];
            letterNames[i][1] = 0;
            k.push_back({static_cast<uint16_t>('A' + i), letterNames[i]});
        }
        static const char* digits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
        for (int i = 0; i < 10; ++i) k.push_back({static_cast<uint16_t>('0' + i), digits[i]});
        static const char* fnames[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
        for (int i = 0; i < 12; ++i) k.push_back({static_cast<uint16_t>(0x70 + i), fnames[i]});
        const KeyOption rest[] = {{0x20, "Space"}, {0x0D, "Enter"}, {0x1B, "Escape"}, {0x09, "Tab"}, {0x08, "Backspace"},
                                  {0xA0, "Left Shift"}, {0xA2, "Left Ctrl"}, {0xA4, "Left Alt"},
                                  {0x25, "Left arrow"}, {0x26, "Up arrow"}, {0x27, "Right arrow"}, {0x28, "Down arrow"},
                                  {0x2D, "Insert"}, {0x2E, "Delete"}, {0x24, "Home"}, {0x23, "End"},
                                  {0x21, "Page up"}, {0x22, "Page down"}};
        for (const auto& r : rest) k.push_back(r);
        return k;
    }();
    return keys;
}

inline const char* KeyName(uint16_t vk) {
    for (const auto& k : Keys())
        if (k.vk == vk) return k.name;
    return "";
}

inline const char* MouseName(uint16_t code) {
    switch (code) {
    case 1: return "Left click";
    case 2: return "Right click";
    case 3: return "Middle click";
    case 4: return "Wheel up";
    case 5: return "Wheel down";
    default: return "";
    }
}

inline std::string BindingLabel(const Binding& b) {
    if (b.action == Action::Key) return KeyName(b.code);
    if (b.action == Action::Mouse) return MouseName(b.code);
    return "Not mapped";
}

// Saved as "none", "key:Space" or "mouse:Left click".
inline std::string EncodeBinding(const Binding& b) {
    if (b.action == Action::Key && *KeyName(b.code)) return std::string("key:") + KeyName(b.code);
    if (b.action == Action::Mouse && *MouseName(b.code)) return std::string("mouse:") + MouseName(b.code);
    return "none";
}

inline Binding DecodeBinding(const std::string& s) {
    Binding b;
    if (s.rfind("key:", 0) == 0) {
        for (const auto& k : Keys())
            if (s.compare(4, std::string::npos, k.name) == 0) return {Action::Key, k.vk};
    } else if (s.rfind("mouse:", 0) == 0) {
        for (uint16_t c = 1; c <= 5; ++c)
            if (s.compare(6, std::string::npos, MouseName(c)) == 0) return {Action::Mouse, c};
    }
    return b;
}

struct Mapping {
    bool enabled = false;
    std::array<Binding, kButtonCount> buttons{};
    bool rightStickMouse = false;  // the right stick moves the mouse
    int mouseSpeed = 10;           // 1..30: pixels per 8 ms at full throw
    int deadzone = 15;             // 0..50 (% of the throw)

    bool AnyMapped() const {
        for (const auto& b : buttons)
            if (b.action != Action::None) return true;
        return rightStickMouse;
    }
    bool operator==(const Mapping& o) const {
        return enabled == o.enabled && buttons == o.buttons && rightStickMouse == o.rightStickMouse &&
               mouseSpeed == o.mouseSpeed && deadzone == o.deadzone;
    }
    bool operator!=(const Mapping& o) const { return !(*this == o); }
};

struct Event {
    Binding binding;
    bool down;
};

// What changed between two button states, as presses and releases of the mapped keys.
inline std::vector<Event> Diff(const Mapping& m, uint32_t before, uint32_t after) {
    std::vector<Event> out;
    for (int b = 0; b < kButtonCount; ++b) {
        const bool was = before & Bit(b), now = after & Bit(b);
        if (was == now || m.buttons[static_cast<size_t>(b)].action == Action::None) continue;
        out.push_back({m.buttons[static_cast<size_t>(b)], now});
    }
    return out;
}

// Accumulates the mouse movement of a stick so slow pushes still move (whole pixels come out).
class MouseStick {
public:
    // `dtMs`: time since the last call.
    void Step(const Mapping& m, uint8_t rx, uint8_t ry, double dtMs, int* dx, int* dy) {
        const float dz = static_cast<float>(m.deadzone) / 100.f;
        const float k = static_cast<float>(m.mouseSpeed * dtMs / 8.0);
        // A little curve: fine control near the middle, full speed at the edge.
        auto curve = [](float v) { return v * std::fabs(v); };
        fx_ += curve(StickAxis(rx, dz)) * k;
        fy_ += curve(StickAxis(ry, dz)) * k;
        *dx = static_cast<int>(fx_);
        *dy = static_cast<int>(fy_);
        fx_ -= static_cast<float>(*dx);
        fy_ -= static_cast<float>(*dy);
    }
    void Reset() { fx_ = fy_ = 0; }

private:
    float fx_ = 0, fy_ = 0;
};

}  // namespace luma::app::pad
