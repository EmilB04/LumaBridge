// The Logitech LED SDK (LogitechLEDLib.h, SDK 8.x/9.x) export surface.
//
// One X-macro list drives the function-pointer typedefs, the GetProcAddress table and the
// pure pass-through exports, so adding an export means editing exactly one line here
// (plus exports.def). Enum parameters (LogiLed::KeyName, LogiLed::DeviceType) are passed
// as int: that is their ABI on MSVC, and we never need the symbolic names in the proxy.
//
// Calling convention: the SDK header declares no convention, so it is the compiler default,
// __cdecl. That only matters for the x86 build; x64 has a single convention.
#pragma once

#define LOGILED_CALL __cdecl

// LOGI_LED_BITMAP_SIZE = 21 * 6 * 4
#define LOGI_LED_BITMAP_SIZE 504

// Functions whose calls LumaBridge inspects (fan-out to Aura) -- exports written by hand.
// X(return type, name, (parameter list), (argument list))
#define LOGILED_INTERCEPTED(X)                                                              \
    X(bool, LogiLedInit, (), ())                                                            \
    X(bool, LogiLedInitWithName, (const char name[]), (name))                               \
    X(bool, LogiLedSaveCurrentLighting, (), ())                                             \
    X(bool, LogiLedSetLighting, (int redPercentage, int greenPercentage, int bluePercentage), \
      (redPercentage, greenPercentage, bluePercentage))                                    \
    X(bool, LogiLedRestoreLighting, (), ())                                                 \
    X(bool, LogiLedFlashLighting,                                                           \
      (int redPercentage, int greenPercentage, int bluePercentage, int milliSecondsDuration, \
       int milliSecondsInterval),                                                           \
      (redPercentage, greenPercentage, bluePercentage, milliSecondsDuration,                \
       milliSecondsInterval))                                                               \
    X(bool, LogiLedPulseLighting,                                                           \
      (int redPercentage, int greenPercentage, int bluePercentage, int milliSecondsDuration, \
       int milliSecondsInterval),                                                           \
      (redPercentage, greenPercentage, bluePercentage, milliSecondsDuration,                \
       milliSecondsInterval))                                                               \
    X(bool, LogiLedStopEffects, (), ())                                                     \
    X(bool, LogiLedSetLightingFromBitmap, (unsigned char bitmap[]), (bitmap))               \
    X(bool, LogiLedSetLightingForTargetZone,                                                \
      (int deviceType, int zone, int redPercentage, int greenPercentage, int bluePercentage), \
      (deviceType, zone, redPercentage, greenPercentage, bluePercentage))                  \
    X(void, LogiLedShutdown, (), ())

// Functions that are forwarded verbatim. Per-key calls land here for now; the Azoth
// per-key milestone moves them to LOGILED_INTERCEPTED.
#define LOGILED_PASSTHROUGH(X)                                                              \
    X(bool, LogiLedGetSdkVersion, (int* majorNum, int* minorNum, int* buildNum),            \
      (majorNum, minorNum, buildNum))                                                       \
    X(bool, LogiLedGetConfigOptionNumber, (const wchar_t* configPath, double* defaultValue), \
      (configPath, defaultValue))                                                           \
    X(bool, LogiLedGetConfigOptionBool, (const wchar_t* configPath, bool* defaultValue),    \
      (configPath, defaultValue))                                                           \
    X(bool, LogiLedGetConfigOptionColor,                                                    \
      (const wchar_t* configPath, int* defaultRed, int* defaultGreen, int* defaultBlue),    \
      (configPath, defaultRed, defaultGreen, defaultBlue))                                  \
    X(bool, LogiLedGetConfigOptionRect,                                                     \
      (const wchar_t* configPath, int* defaultX, int* defaultY, int* defaultWidth,          \
       int* defaultHeight),                                                                 \
      (configPath, defaultX, defaultY, defaultWidth, defaultHeight))                        \
    X(bool, LogiLedGetConfigOptionString,                                                   \
      (const wchar_t* configPath, wchar_t* defaultValue, int bufferSize),                   \
      (configPath, defaultValue, bufferSize))                                               \
    X(bool, LogiLedGetConfigOptionKeyInput,                                                 \
      (const wchar_t* configPath, wchar_t* defaultValue, int bufferSize),                   \
      (configPath, defaultValue, bufferSize))                                               \
    X(bool, LogiLedGetConfigOptionSelect,                                                   \
      (const wchar_t* configPath, wchar_t* defaultValue, int* valueSize,                    \
       const wchar_t* values, int bufferSize),                                              \
      (configPath, defaultValue, valueSize, values, bufferSize))                            \
    X(bool, LogiLedGetConfigOptionRange,                                                    \
      (const wchar_t* configPath, int* defaultValue, int min, int max),                     \
      (configPath, defaultValue, min, max))                                                 \
    X(bool, LogiLedSetConfigOptionLabel, (const wchar_t* configPath, wchar_t* label),       \
      (configPath, label))                                                                  \
    X(bool, LogiLedSetTargetDevice, (int targetDevice), (targetDevice))                     \
    X(bool, LogiLedSetLightingForKeyWithScanCode,                                           \
      (int keyCode, int redPercentage, int greenPercentage, int bluePercentage),            \
      (keyCode, redPercentage, greenPercentage, bluePercentage))                            \
    X(bool, LogiLedSetLightingForKeyWithHidCode,                                            \
      (int keyCode, int redPercentage, int greenPercentage, int bluePercentage),            \
      (keyCode, redPercentage, greenPercentage, bluePercentage))                            \
    X(bool, LogiLedSetLightingForKeyWithQuartzCode,                                         \
      (int keyCode, int redPercentage, int greenPercentage, int bluePercentage),            \
      (keyCode, redPercentage, greenPercentage, bluePercentage))                            \
    X(bool, LogiLedSetLightingForKeyWithKeyName,                                            \
      (int keyName, int redPercentage, int greenPercentage, int bluePercentage),            \
      (keyName, redPercentage, greenPercentage, bluePercentage))                            \
    X(bool, LogiLedSaveLightingForKey, (int keyName), (keyName))                            \
    X(bool, LogiLedRestoreLightingForKey, (int keyName), (keyName))                         \
    X(bool, LogiLedExcludeKeysFromBitmap, (int* keyList, int listCount), (keyList, listCount)) \
    X(bool, LogiLedFlashSingleKey,                                                          \
      (int keyName, int redPercentage, int greenPercentage, int bluePercentage,             \
       int msDuration, int msInterval),                                                     \
      (keyName, redPercentage, greenPercentage, bluePercentage, msDuration, msInterval))    \
    X(bool, LogiLedPulseSingleKey,                                                          \
      (int keyName, int startRedPercentage, int startGreenPercentage,                       \
       int startBluePercentage, int finishRedPercentage, int finishGreenPercentage,         \
       int finishBluePercentage, int msDuration, bool isInfinite),                          \
      (keyName, startRedPercentage, startGreenPercentage, startBluePercentage,              \
       finishRedPercentage, finishGreenPercentage, finishBluePercentage, msDuration,        \
       isInfinite))                                                                         \
    X(bool, LogiLedStopEffectsOnKey, (int keyName), (keyName))

#define LOGILED_ALL(X) LOGILED_INTERCEPTED(X) LOGILED_PASSTHROUGH(X)
