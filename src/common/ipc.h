// Game DLL -> LumaBridge app messages.
//
// When the LumaBridge app is running it is the only process that talks to Aura: the SDK
// front-ends inside games (LogiLed proxy, Chroma/Corsair/LightFX emulators) send it their
// current color with WM_COPYDATA instead, so manual colors, several games and GameSense
// never fight over Aura. Without the app, each front-end drives Aura itself.
//
// Frames are small fixed-size POD structs; the layout is versioned so an older DLL left in a
// game folder can't confuse a newer app.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace luma::ipc {

// Window class of the app's (hidden) message window. Stable: DLLs FindWindow() it.
constexpr wchar_t kAppWindowClass[] = L"LumaBridgeAppWnd";

// COPYDATASTRUCT::dwData tag ("LBv1").
constexpr unsigned long kCopyDataTag = 0x4C427631;

enum class FrameKind : uint32_t {
    Color = 1,    // current ambient color of this source
    Release = 2,  // source is done (SDK shut down); forget it
};

struct Frame {
    uint32_t size;  // sizeof(Frame), checked by the receiver
    FrameKind kind;
    uint32_t pid;   // sending process
    uint8_t r, g, b, reserved;
    char source[32];  // SDK name, e.g. "Logitech LIGHTSYNC"
};

inline Frame MakeFrame(FrameKind kind, uint32_t pid, uint8_t r, uint8_t g, uint8_t b,
                       const char* source) {
    Frame f{};
    f.size = sizeof(Frame);
    f.kind = kind;
    f.pid = pid;
    f.r = r;
    f.g = g;
    f.b = b;
    if (source) {
        const size_t n = std::min(std::strlen(source), sizeof f.source - 1);
        std::memcpy(f.source, source, n);  // f is zero-initialized: stays terminated
    }
    return f;
}

// Validates an incoming buffer; returns false for foreign or truncated messages.
inline bool ParseFrame(const void* data, size_t size, Frame* out) {
    if (!data || size != sizeof(Frame)) return false;
    std::memcpy(out, data, sizeof(Frame));
    if (out->size != sizeof(Frame)) return false;
    if (out->kind != FrameKind::Color && out->kind != FrameKind::Release) return false;
    out->source[sizeof out->source - 1] = '\0';
    return true;
}

}  // namespace luma::ipc
