// The subset of Razer's RzChromaSDKTypes.h / RzErrors.h the emulator needs, rewritten from
// the public Chroma SDK documentation so no Razer headers are required.
//
// Only the *layouts we read* matter for safety: every read of an effect parameter is bounded
// by the smallest struct size that effect type can have. Enum values below are the SDK 2.x/3.x
// values; if a game renders wrong colors, check these first against the current
// RzChromaSDKTypes.h (see docs/sdk-emulators.md).
#pragma once

#include <cstdint>

namespace luma::chroma {

using RZRESULT = long;

constexpr RZRESULT RZRESULT_SUCCESS = 0;
constexpr RZRESULT RZRESULT_INVALID_PARAMETER = 87;
constexpr RZRESULT RZRESULT_NOT_SUPPORTED = 50;
constexpr RZRESULT RZRESULT_NOT_FOUND = 1168;
constexpr RZRESULT RZRESULT_SERVICE_NOT_ACTIVE = 1062;
constexpr RZRESULT RZRESULT_FAILED = -1;

// Device classes Chroma effects are addressed to (not specific models).
enum class DeviceClass { Keyboard, Mouse, Headset, Mousepad, Keypad, ChromaLink, Count };

// --- Keyboard (ChromaSDK::Keyboard) ---------------------------------------------------
namespace keyboard {
constexpr int kMaxRow = 6;
constexpr int kMaxColumn = 22;
constexpr int kMaxRowExt = 8;
constexpr int kMaxColumnExt = 24;
enum EffectType : int {
    NONE = 0,
    BREATHING = 1,  // deprecated
    CUSTOM = 2,     // COLORREF Color[6][22]
    REACTIVE = 3,   // deprecated
    STATIC = 4,     // COLORREF Color
    SPECTRUMCYCLING = 5,
    WAVE = 6,
    RESERVED = 7,
    CUSTOM_KEY = 8,  // COLORREF Color[6][22]; COLORREF Key[6][22]
    CUSTOM2 = 9,     // COLORREF Color[8][24]; COLORREF Key[6][22] (SDK 3.x)
};
}  // namespace keyboard

// --- Mouse (ChromaSDK::Mouse) ---------------------------------------------------------
namespace mouse {
constexpr int kMaxRow = 9;
constexpr int kMaxColumn = 7;
enum EffectType : int {
    NONE = 0,
    BLINKING = 1,
    BREATHING = 2,
    CUSTOM = 3,  // COLORREF Color[30] (RZLED indexed)
    REACTIVE = 4,
    SPECTRUMCYCLING = 5,
    STATIC = 6,  // struct { RZLED LEDId; COLORREF Color; }
    WAVE = 7,
    CUSTOM2 = 8,  // COLORREF Color[9][7]
};
constexpr int kMaxLeds = 30;
}  // namespace mouse

// --- Headset (ChromaSDK::Headset) -----------------------------------------------------
namespace headset {
constexpr int kMaxLeds = 5;
enum EffectType : int { NONE = 0, STATIC = 1, BREATHING = 2, SPECTRUMCYCLING = 3, CUSTOM = 4 };
}  // namespace headset

// --- Mousepad (ChromaSDK::Mousepad) ---------------------------------------------------
namespace mousepad {
constexpr int kMaxLeds = 15;
enum EffectType : int { NONE = 0, BREATHING = 1, CUSTOM = 2, SPECTRUMCYCLING = 3, STATIC = 4, WAVE = 5, CUSTOM2 = 6 };
constexpr int kMaxLeds2 = 20;
}  // namespace mousepad

// --- Keypad (ChromaSDK::Keypad) -------------------------------------------------------
namespace keypad {
constexpr int kMaxRow = 4;
constexpr int kMaxColumn = 5;
enum EffectType : int { NONE = 0, BREATHING = 1, CUSTOM = 2, REACTIVE = 3, SPECTRUMCYCLING = 4, STATIC = 5, WAVE = 6 };
}  // namespace keypad

// --- Chroma Link (ChromaSDK::ChromaLink) ----------------------------------------------
namespace chromalink {
constexpr int kMaxLeds = 5;
enum EffectType : int { NONE = 0, CUSTOM = 1, STATIC = 2 };
}  // namespace chromalink

// --- Generic CreateEffect (ChromaSDK::EFFECT_TYPE) ------------------------------------
namespace generic {
enum EffectType : int {
    NONE = 0,
    WAVE = 1,
    SPECTRUMCYCLING = 2,
    BREATHING = 3,
    BLINKING = 4,
    REACTIVE = 5,
    STATIC = 6,  // STATIC_EFFECT_TYPE { DWORD Size; DWORD Param; COLORREF Color; }
    CUSTOM = 7,  // device specific
};
}  // namespace generic

// DEVICE_INFO_TYPE for QueryDevice.
struct DeviceInfo {
    int deviceType;  // DEVICE_KEYBOARD = 1, MOUSE = 2, HEADSET = 3, MOUSEPAD = 4, KEYPAD = 5, SYSTEM = 6, SPEAKERS = 7
    unsigned long connected;
};

}  // namespace luma::chroma
