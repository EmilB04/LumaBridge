// The subset of Corsair's CUESDK.h (iCUE SDK 2.x / 3.x "CUE SDK") the emulator implements,
// re-declared so no Corsair headers are needed. Enums are int in the ABI.
//
// SDK 4.x (iCUESDK.x64_2019.dll, CorsairConnect / session callbacks, string device ids) is a
// different, incompatible ABI and is not emulated; see docs/sdk-emulators.md.
#pragma once

namespace luma::corsair {

enum CorsairError : int {
    CE_Success = 0,
    CE_ServerNotFound = 1,
    CE_NoControl = 2,
    CE_ProtocolHandshakeMissing = 3,
    CE_IncompatibleProtocol = 4,
    CE_InvalidArguments = 5,
};

enum CorsairDeviceType : int {
    CDT_Unknown = 0,
    CDT_Mouse = 1,
    CDT_Keyboard = 2,
    CDT_Headset = 3,
    CDT_MouseMat = 4,
    CDT_HeadsetStand = 5,
    CDT_CommanderPro = 6,
    CDT_LightingNodePro = 7,
    CDT_MemoryModule = 8,
    CDT_Cooler = 9,
    CDT_Motherboard = 10,
    CDT_GraphicsCard = 11,
};

enum CorsairPhysicalLayout : int {
    CPL_Invalid = 0,
    CPL_US = 1,
    CPL_UK = 2,
    CPL_BR = 3,
    CPL_JP = 4,
    CPL_KR = 5,
    CPL_Zones1 = 6,
    CPL_Zones2 = 7,
    CPL_Zones3 = 8,
    CPL_Zones4 = 9,
};

enum CorsairLogicalLayout : int { CLL_Invalid = 0, CLL_US_Int = 1 };

enum CorsairDeviceCaps : int { CDC_None = 0, CDC_Lighting = 1, CDC_PropertyLookup = 2 };

struct CorsairProtocolDetails {
    const char* sdkVersion;
    const char* serverVersion;
    int sdkProtocolVersion;
    int serverProtocolVersion;
    bool breakingChanges;
};

struct CorsairChannelDeviceInfo {
    int type;
    int deviceLedCount;
};

struct CorsairChannelInfo {
    int totalLedsCount;
    int devicesCount;
    CorsairChannelDeviceInfo* devices;
};

struct CorsairChannelsInfo {
    int channelsCount;
    CorsairChannelInfo* channels;
};

// SDK 3.x layout. SDK 2.x headers stop after capsMask; since the game only reads through
// the pointer we return, the longer struct is backward compatible.
struct CorsairDeviceInfo {
    int type;  // CorsairDeviceType
    const char* model;
    int physicalLayout;  // CorsairPhysicalLayout
    int logicalLayout;   // CorsairLogicalLayout
    int capsMask;        // CorsairDeviceCaps
    int ledsCount;
    CorsairChannelsInfo channels;
    const char* deviceId;
};

struct CorsairLedPosition {
    int ledId;
    double top;
    double left;
    double height;
    double width;
};

struct CorsairLedPositions {
    int numberOfLed;
    CorsairLedPosition* pLedPosition;
};

struct CorsairLedColor {
    int ledId;
    int r;
    int g;
    int b;
};

}  // namespace luma::corsair
