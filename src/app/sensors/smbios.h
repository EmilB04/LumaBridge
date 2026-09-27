// SMBIOS (the firmware's hardware table, from GetSystemFirmwareTable('RSMB')): board, BIOS,
// CPU and memory module names for the dashboard. Pure parser, tested.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace luma::app::sensors {

struct MemoryModule {
    std::string slot;          // "DIMM_A2"
    std::string manufacturer;  // "Kingston"
    std::string part;          // "KF3600C17D4/8GX"
    uint32_t sizeMb = 0;
    uint32_t speedMts = 0;     // configured speed if known, else rated
};

struct SmbiosInfo {
    std::string boardMaker, boardName;  // "ASUSTeK COMPUTER INC.", "ROG STRIX B550-F GAMING"
    std::string biosVendor, biosVersion, biosDate;
    std::string cpu;                    // processor version string
    std::vector<MemoryModule> memory;   // populated slots only
};

namespace detail {
inline std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}
}  // namespace detail

// `raw`: the RawSMBIOSData blob (8-byte header, then the structure table).
inline SmbiosInfo ParseSmbios(const std::vector<uint8_t>& raw) {
    SmbiosInfo out;
    if (raw.size() < 8) return out;
    const uint32_t len = raw[4] | raw[5] << 8 | raw[6] << 16 | static_cast<uint32_t>(raw[7]) << 24;
    const size_t end = std::min<size_t>(raw.size(), 8 + static_cast<size_t>(len));
    size_t pos = 8;
    while (pos + 4 <= end) {
        const uint8_t type = raw[pos];
        const uint8_t flen = raw[pos + 1];
        if (flen < 4 || pos + flen > end) break;
        // Strings follow the formatted area, each NUL-terminated; the set ends with an extra
        // NUL (a structure without strings has just two NULs).
        std::vector<std::string> strings;
        size_t p = pos + flen, next;
        if (p + 1 < end && raw[p] == 0 && raw[p + 1] == 0) {
            next = p + 2;
        } else {
            while (p < end) {
                std::string str;
                while (p < end && raw[p]) str += static_cast<char>(raw[p++]);
                strings.push_back(str);
                ++p;  // its NUL
                if (p < end && raw[p] == 0) {
                    ++p;  // end of the set
                    break;
                }
            }
            next = p;
        }
        const uint8_t* f = raw.data() + pos;
        auto str = [&](size_t off) -> std::string {
            if (off >= flen) return {};
            const uint8_t idx = f[off];
            return idx && idx <= strings.size() ? detail::Trim(strings[idx - 1]) : std::string();
        };
        auto word = [&](size_t off) -> uint32_t { return off + 1 < flen ? (f[off] | f[off + 1] << 8) : 0; };
        if (type == 0) {
            out.biosVendor = str(0x04);
            out.biosVersion = str(0x05);
            out.biosDate = str(0x08);
        } else if (type == 2 && out.boardName.empty()) {
            out.boardMaker = str(0x04);
            out.boardName = str(0x05);
        } else if (type == 4 && out.cpu.empty()) {
            out.cpu = str(0x10);
        } else if (type == 17) {
            uint32_t size = word(0x0C);
            if (size == 0x7FFF && flen > 0x1F)  // extended size (MB) for >= 32 GB modules
                size = f[0x1C] | f[0x1D] << 8 | f[0x1E] << 16 | static_cast<uint32_t>(f[0x1F] & 0x7F) << 24;
            else if (size != 0xFFFF && (size & 0x8000))  // given in KB
                size = (size & 0x7FFF) / 1024;
            if (size && size != 0xFFFF) {
                MemoryModule m;
                m.sizeMb = size;
                m.slot = str(0x10);
                m.manufacturer = str(0x17);
                m.part = str(0x1A);
                const uint32_t configured = word(0x20), rated = word(0x15);
                m.speedMts = configured ? configured : rated;
                out.memory.push_back(m);
            }
        } else if (type == 127) {
            break;  // end-of-table
        }
        if (next <= pos) break;
        pos = next;
    }
    return out;
}

}  // namespace luma::app::sensors
