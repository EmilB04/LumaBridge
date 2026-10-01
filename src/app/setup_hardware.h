// What the "Your setup" preview draws, found by scanning the system (SMBIOS): the board's
// look from its model, and the memory's look and filled slots from the modules. Pure, tested.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "friendly_names.h"
#include "smbios.h"

namespace luma::app {

enum class BoardStyle {
    Generic,
    Rog,       // ASUS ROG (Crosshair, Maximus, Hero, ...): a lit logo on the I/O cover
    RogStrix,  // ASUS ROG Strix: drawn like Rog for now
    Tuf,       // ASUS TUF Gaming: the TUF badge on the I/O cover
};

enum class RamStyle {
    Generic,       // plain sticks, lit end to end
    KingstonFury,  // Kingston FURY Beast / Renegade RGB: blocks, a long bar, the FURY badge, stripes
    HyperXFury,    // HyperX FURY RGB: one light bar with angled cuts
};

struct SetupHardware {
    BoardStyle board = BoardStyle::Generic;
    std::string boardName;  // "ASUS ROG STRIX B550-F GAMING", "" if unknown
    RamStyle ram = RamStyle::Generic;
    std::string ramName;    // "Kingston FURY", "" if unknown
    // Slots from the CPU outward (A1, A2, B1, B2), whether each holds a stick. `slotsKnown`:
    // read from the modules' locators; else only `sticks` is known.
    std::array<bool, 4> slots{};
    bool slotsKnown = false;
    int sticks = 0;
};

namespace detail {
inline std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace detail

// A module's slot (0 A1, 1 A2, 2 B1, 3 B2) from its SMBIOS locator: "DIMM_A2", "A2",
// "ChannelB-DIMM0", "P0 CHANNEL A / DIMM 1" (the bank locator, then the device locator).
// "DIMM0" / "DIMM1" count from zero; "DIMM 1" / "DIMM 2" from one, unless `zeroBased` (the
// board also has a "DIMM 0"). -1 if it can't tell.
inline int SlotIndex(const std::string& locator, bool zeroBased = false) {
    const std::string s = detail::Upper(locator);
    int channel = -1, dimm = -1;
    const size_t ch = s.find("CHANNEL");
    if (ch != std::string::npos) {
        // "CHANNEL A", "ChannelB", or numbered from zero: "P0_Node0_Channel0_Dimm0".
        for (size_t i = ch + 7; i < s.size(); ++i) {
            if (s[i] == ' ' || s[i] == '_' || s[i] == '-') continue;
            if (s[i] >= 'A' && s[i] <= 'H') channel = s[i] - 'A';
            else if (std::isdigit(static_cast<unsigned char>(s[i]))) channel = s[i] - '0';
            break;
        }
        const size_t d = s.find("DIMM", ch);
        if (d != std::string::npos && d + 4 < s.size()) {
            const bool spaced = s[d + 4] == ' ' && !zeroBased;
            for (size_t i = d + 4; i < s.size(); ++i)
                if (std::isdigit(static_cast<unsigned char>(s[i]))) {
                    dimm = s[i] - '0' - (spaced ? 1 : 0);
                    break;
                }
        }
    } else {
        // "DIMM_A2", "A2": a channel letter followed by 1 or 2.
        for (size_t i = 0; i + 1 < s.size(); ++i)
            if ((s[i] == 'A' || s[i] == 'B') && (s[i + 1] == '1' || s[i + 1] == '2') &&
                (i == 0 || !std::isalpha(static_cast<unsigned char>(s[i - 1])))) {
                channel = s[i] - 'A';
                dimm = s[i + 1] - '1';
                break;
            }
    }
    if (channel < 0 || channel > 1 || dimm < 0 || dimm > 1) return -1;
    return channel * 2 + dimm;
}

inline SetupHardware DetectSetup(const sensors::SmbiosInfo& info) {
    SetupHardware h;
    h.boardName = sensors::FriendlyBoard(info.boardMaker, info.boardName);
    const std::string board = detail::Upper(info.boardName);
    if (board.find("STRIX") != std::string::npos) h.board = BoardStyle::RogStrix;
    else if (board.find("ROG") != std::string::npos || board.find("CROSSHAIR") != std::string::npos ||
             board.find("MAXIMUS") != std::string::npos)
        h.board = BoardStyle::Rog;
    else if (board.find("TUF") != std::string::npos) h.board = BoardStyle::Tuf;

    h.sticks = static_cast<int>(info.memory.size());
    bool allKnown = !info.memory.empty();
    // Some boards (AMD) name the channel in the bank locator ("P0 CHANNEL A") and the slot in
    // the device locator ("DIMM 0" / "DIMM 1"); a "DIMM 0" anywhere means they count from zero.
    bool zeroBased = false;
    for (const auto& m : info.memory)
        zeroBased = zeroBased || detail::Upper(m.slot + " " + m.bank).find("DIMM 0") != std::string::npos;
    for (const auto& m : info.memory) {
        int slot = SlotIndex(m.slot, zeroBased);
        if (slot < 0 && !m.bank.empty()) slot = SlotIndex(m.bank + " / " + m.slot, zeroBased);
        if (slot < 0 || h.slots[static_cast<size_t>(slot)]) allKnown = false;
        else h.slots[static_cast<size_t>(slot)] = true;
    }
    // Last: slots numbered in a row without channels ("DIMM 1" .. "DIMM 4", "DIMM0" ..
    // "DIMM3"), in the board's order A1 A2 B1 B2. Only when every stick has a distinct number.
    if (!allKnown && !info.memory.empty()) {
        std::array<int, 8> nums{};
        size_t n = 0;
        bool ok = info.memory.size() <= 4;
        for (const auto& m : info.memory) {
            const std::string s = detail::Upper(m.slot);
            const size_t d = s.find("DIMM");
            int num = -1;
            if (d != std::string::npos && s.find_first_of("ABCH", d + 4) == std::string::npos)
                for (size_t i = d + 4; i < s.size() && num < 0; ++i) {
                    if (s[i] == ' ' || s[i] == '_' || s[i] == '-') continue;
                    if (std::isdigit(static_cast<unsigned char>(s[i]))) num = s[i] - '0';
                    else break;
                }
            if (num < 0 || num > 4) ok = false;
            else nums[n++] = num;
        }
        int lowest = 9;
        for (size_t i = 0; i < n; ++i) lowest = std::min(lowest, nums[i]);
        const int base = lowest == 0 ? 0 : 1;
        std::array<bool, 4> seq{};
        for (size_t i = 0; ok && i < n; ++i) {
            const int slot = nums[i] - base;
            if (slot < 0 || slot > 3 || seq[static_cast<size_t>(slot)]) ok = false;
            else seq[static_cast<size_t>(slot)] = true;
        }
        if (ok) {
            h.slots = seq;
            allKnown = true;
        }
    }
    h.slotsKnown = allKnown;
    if (!allKnown) h.slots = {};
    if (!info.memory.empty()) {
        const auto& m = info.memory.front();
        h.ramName = sensors::FriendlyMemory(m.manufacturer, m.part);
        if (h.ramName.rfind("Kingston FURY", 0) == 0) h.ram = RamStyle::KingstonFury;
        else if (h.ramName.rfind("HyperX FURY", 0) == 0) h.ram = RamStyle::HyperXFury;
    }
    return h;
}

// The sticks, identical ones grouped: "2 x 8 GB HyperX FURY @ 3600 MT/s".
inline std::vector<std::string> MemoryGroups(const sensors::SmbiosInfo& info) {
    std::vector<std::pair<std::string, int>> groups;
    for (const auto& m : info.memory) {
        const std::string text = std::to_string(m.sizeMb / 1024) + " GB " + sensors::FriendlyMemory(m.manufacturer, m.part) +
                                 " @ " + std::to_string(m.speedMts) + " MT/s";
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == text; });
        if (it != groups.end()) ++it->second;
        else groups.push_back({text, 1});
    }
    std::vector<std::string> out;
    for (const auto& g : groups) out.push_back(std::to_string(g.second) + " x " + g.first);
    return out;
}

// The sticks in one short line, for a list: "3 sticks, 32 GB @ 3600 MT/s" (the speed only when
// every stick runs at it). "" with no sticks.
inline std::string MemorySummary(const sensors::SmbiosInfo& info) {
    if (info.memory.empty()) return "";
    uint64_t mb = 0;
    bool sameSpeed = true;
    for (const auto& m : info.memory) {
        mb += m.sizeMb;
        sameSpeed = sameSpeed && m.speedMts == info.memory.front().speedMts;
    }
    const size_t n = info.memory.size();
    std::string out = std::to_string(n) + (n == 1 ? " stick, " : " sticks, ") + std::to_string(mb / 1024) + " GB";
    if (sameSpeed && info.memory.front().speedMts) out += " @ " + std::to_string(info.memory.front().speedMts) + " MT/s";
    return out;
}

// Which slots to draw filled for `sticks` sticks when the locators aren't known: A2 / B2
// first, as most boards want.
inline std::array<bool, 4> GuessSlots(int sticks) {
    static const int kOrder[4] = {1, 3, 0, 2};
    std::array<bool, 4> s{};
    for (int i = 0; i < sticks && i < 4; ++i) s[static_cast<size_t>(kOrder[i])] = true;
    return s;
}

}  // namespace luma::app
