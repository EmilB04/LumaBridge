// Reading the board's sensors without LibreHardwareMonitor: the CPU's own temperature
// (AMD Ryzen, family 17h and later) and the Nuvoton NCT679x monitoring chip most ASUS AMD
// boards use (fan speeds, board temperatures). Register facts as documented by the Linux
// k10temp / nct6775 drivers and LibreHardwareMonitor; the code is LumaBridge's own. Only
// reads: the chip is never configured, apart from selecting which register to read.
// Pure, tested. The reading itself happens in the elevated helper (src/helper).
#pragma once

#include <cstdint>

namespace luma::app::hw {

// ---- AMD Ryzen ----------------------------------------------------------------------

// SMN address of THM_TCON_CUR_TMP, the current control temperature (Tctl).
constexpr uint32_t kTctlSmn = 0x00059800;

// Tctl in °C from THM_TCON_CUR_TMP: bits 31:21 in 1/8 °C; 49 °C lower when the range-select
// bit (19) or both TJ_SEL bits (17:16) are set.
inline double TctlCelsius(uint32_t reg) {
    const bool shifted = (reg & 0x80000u) != 0 || (reg & 0x30000u) == 0x30000u;
    const double t = static_cast<double>(reg >> 21) * 0.125;
    return shifted ? t - 49.0 : t;
}

// ---- Nuvoton NCT679x ----------------------------------------------------------------

constexpr uint8_t kSuperIoEnter = 0x87;  // written twice to the config port
constexpr uint8_t kSuperIoExit = 0xAA;
constexpr uint8_t kChipIdRegister = 0x20, kChipRevisionRegister = 0x21;
constexpr uint8_t kLogicalDeviceRegister = 0x07, kHardwareMonitorLdn = 0x0B;
constexpr uint8_t kBaseAddressRegister = 0x60;
// Hardware-monitor registers are banked: bank at 0x4E, then address / data at base + 5 / 6.
constexpr uint8_t kBankSelect = 0x4E, kAddressOffset = 0x05, kDataOffset = 0x06;
constexpr uint16_t kVendorHigh = 0x804F, kVendorLow = 0x004F, kNuvotonVendor = 0x5CA3;

// Chips with the register layout below (NCT6796D and later); nullptr for anything else.
inline const char* NuvotonChip(uint8_t id, uint8_t revision) {
    if (id == 0xD4) {
        switch (revision) {
        case 0x23: return "NCT6796D";
        case 0x2A: return "NCT6796D-R";
        case 0x51: return "NCT6797D";
        case 0x2B: return "NCT6798D";
        default: return nullptr;
        }
    }
    if (id == 0xD8 && revision == 0x02) return "NCT6799D";
    return nullptr;
}

// Fan speed from the 13-bit count (high byte at the register, low 5 bits at register + 1).
// 0 when stopped or nothing is connected.
inline double FanRpm(uint8_t high, uint8_t low) {
    const int count = (high << 5) | (low & 0x1F);
    if (count >= 0x1FFF || count < 0x15) return 0;
    return 1.35e6 / count;
}

// An 8-bit temperature register (°C, signed); false for "no sensor" readings.
inline bool TemperatureValid(uint8_t raw, double* celsius) {
    const int t = static_cast<int8_t>(raw);
    if (t <= -40 || t >= 125) return false;
    *celsius = t;
    return true;
}

struct Input {
    uint16_t reg;  // bank << 8 | register
    const char* name;
};

// Fan inputs in the usual ASUS wiring of these chips (SYSFAN, CPUFAN, AUXFAN0-4).
constexpr Input kFans[] = {{0x4B0, "Chassis Fan 1"},    {0x4B2, "CPU Fan"},  {0x4B4, "Chassis Fan 2"},
                           {0x4B6, "Chassis Fan 3"},    {0x4B8, "CPU Optional Fan"}, {0x4BA, "AIO Pump"},
                           {0x4CC, "Fan 7"}};
// Temperatures with a fixed meaning on these chips.
constexpr Input kTemps[] = {{0x490, "Motherboard"}, {0x491, "CPU Socket"}};

}  // namespace luma::app::hw
