// Shared memory between the LumaBridge app (running as the user) and its hardware helper
// LumaBridge-Helper.exe (a scheduled task running as SYSTEM, set up once from the Devices
// page). The helper creates it with access for signed-in users; the app opens it.
//
//   app -> helper: its pid and heartbeat, whether it wants RAM lighting, the RAM colors;
//   helper -> app: its status, the RAM status, and the sensors it reads (about once a second,
//                  published seqlock-style: sensorSeq is odd while being written).
#pragma once

#include <cstdint>

namespace luma::app::helper {

constexpr wchar_t kSharedName[] = L"Global\\LumaBridgeHelper";
constexpr uint32_t kMagic = 0x5048424C;  // "LBHP"
constexpr uint32_t kVersion = 2;

enum class Status : uint32_t {
    Starting = 0,
    Running,
    NoPawnIO,  // the PawnIO driver isn't installed / can't be opened
};

enum class RamStatus : uint32_t {
    Off = 0,       // not asked for
    Starting,
    Ready,         // found the sticks, waiting for colors
    Active,        // writing colors
    NoModule,      // AMD / Intel SMBus module missing next to the helper
    ModuleFailed,  // PawnIO refused both AMD and Intel SMBus modules
    NotKingston,   // SMBIOS lists no Kingston / HyperX memory
    NoController,  // nothing answers at 0x27
    NoSticks,      // no DDR4 stick answers at 0x50-0x53
    WriteFailed,   // writes kept failing: stopped
    BusBusy,       // couldn't get the shared SMBus lock
};

// What the RAM shows when LumaBridge lets go of it (switched off, Armoury Crate's turn, exit).
enum class RamRelease : uint32_t { OwnRainbow = 0, Off = 1, KeepLast = 2 };

enum class SensorKind : uint32_t { Cpu = 0, Board = 1 };
enum class SensorType : uint32_t { Temperature = 0, Fan = 1, Power = 2 };  // Power: since 0.15.1, same layout

struct SensorEntry {
    SensorKind kind;
    SensorType type;
    double value;   // °C, RPM or W
    char name[40];
};

constexpr int kMaxSensors = 16;
constexpr int kRamLeds = 20;  // 4 slots x 5 LEDs

struct Shared {
    uint32_t magic, version;
    // App -> helper.
    uint32_t appPid;
    uint32_t ramWanted;  // 1: find the RAM and light it when ramOwn
    uint32_t ramOwn;     // 1: show ramColors; 0: stop writing (the sticks keep the last color)
    uint32_t ramSeq;     // bumped by the app when ramColors changes
    uint32_t ramRelease; // what the sticks show when LumaBridge lets go: RamRelease
    uint64_t appBeat;    // bumped by the app a few times a second (the helper watches it change)
    uint8_t ramColors[kRamLeds][3];
    // Helper -> app.
    uint32_t helperPid;
    Status status;
    RamStatus ram;
    uint32_t sticks;     // bit i: a RAM stick in SPD slot i
    uint64_t helperBeat; // bumped by the helper every loop (the app watches it change)
    uint32_t sensorSeq;
    uint32_t sensorCount;
    SensorEntry sensors[kMaxSensors];
    char chip[32];       // the monitoring chip found ("NCT6798D"), or ""
};

}  // namespace luma::app::helper
