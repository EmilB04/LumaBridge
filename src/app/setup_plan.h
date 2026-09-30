// The setup guide: what hardware and lighting software a PC has, and which of LumaBridge's
// connections that turns on. The guide asks; detection (vendor_detect.cpp) only suggests the
// answers. Pure, tested.
#pragma once

#include <array>
#include <cstdint>
#include <cwctype>
#include <string>

namespace luma::app::setup {

// Hardware the guide asks about.
enum class Brand { Asus, Logitech, Razer, SteelSeries, Corsair, Azoth, FuryRam, Alienware, DualSense, Count };
// Lighting software the guide asks about.
enum class App { ArmouryCrate, GHub, Synapse, SteelSeriesGG, Icue, AlienwareCC, Count };
// LumaBridge's connections, in the order the guide lists them.
enum class Conn {
    Lightsync, Chroma, GameSense, AlienFx, LogitechDevices, Azoth, Helper, RamLighting, LampArray, OpenRgb, Handback,
    DualSense, Count
};

constexpr int kBrands = static_cast<int>(Brand::Count);
constexpr int kApps = static_cast<int>(App::Count);
constexpr int kConns = static_cast<int>(Conn::Count);

struct Answers {
    std::array<bool, kBrands> brands{};
    std::array<bool, kApps> apps{};
    bool has(Brand b) const { return brands[static_cast<int>(b)]; }
    bool has(App a) const { return apps[static_cast<int>(a)]; }
    void set(Brand b, bool on = true) { brands[static_cast<int>(b)] = on; }
    void set(App a, bool on = true) { apps[static_cast<int>(a)] = on; }
};

struct BrandInfo {
    const char* name;
    const char* what;  // what LumaBridge lights of it
};
inline const BrandInfo& Info(Brand b) {
    static const BrandInfo k[kBrands] = {
        {"ASUS Aura", "Motherboard, fans and ARGB strips"},
        {"Logitech G", "Mice and other LIGHTSYNC gear"},
        {"Razer", "Chroma devices"},
        {"SteelSeries", "GG / Engine devices"},
        {"Corsair", "iCUE devices"},
        {"ROG Azoth", "The keyboard, key by key"},
        {"Kingston FURY / HyperX RGB", "The memory sticks"},
        {"Alienware", "AlienFX devices"},
        {"Sony DualSense", "The controller's lightbar"},
    };
    return k[static_cast<int>(b)];
}
struct AppInfo {
    const char* name;
    const char* vendor;
};
inline const AppInfo& Info(App a) {
    static const AppInfo k[kApps] = {
        {"Armoury Crate", "ASUS"},        {"G HUB", "Logitech"},     {"Synapse", "Razer"},
        {"GG / Engine", "SteelSeries"},   {"iCUE", "Corsair"},       {"Alienware Command Center", "Alienware"},
    };
    return k[static_cast<int>(a)];
}

struct ConnInfo {
    const char* name;
    const char* why;
    bool admin;    // needs a Windows administrator approval once
    bool install;  // an install (Integrations), not just a switch
    const char* integration;  // its Integrations id, "" for switches
};
inline const ConnInfo& Info(Conn c) {
    static const ConnInfo k[kConns] = {
        {"Logitech LIGHTSYNC games", "Games made for Logitech lighting light your devices too.", false, true, "logitech"},
        {"Razer Chroma games", "The largest catalogue of RGB games.", true, true, "chroma"},
        {"SteelSeries GameSense games", "Built in: games made for SteelSeries lighting.", false, false, ""},
        {"Alienware AlienFX games", "Older games with Alienware lighting.", true, true, "lightfx"},
        {"Logitech devices", "Your Logitech gear follows LumaBridge, through G HUB.", false, false, ""},
        {"ROG Azoth keyboard", "Every key its own color, by cable or the Omni receiver.", false, false, ""},
        {"Hardware access", "Memory lighting, fan speeds and temperatures (the signed PawnIO driver).", true, true,
         "helper"},
        {"Memory lighting", "Kingston FURY / HyperX RGB sticks (needs Hardware access).", false, false, ""},
        {"Windows Dynamic Lighting devices", "Any brand's device with Windows' lighting standard built in, lit directly.",
         false, false, ""},
        {"OpenRGB devices", "Only if you use OpenRGB: what it supports, while it runs with its SDK server on.", false, false,
         ""},
        {"Armoury Crate hand-back", "Gives the lights back to Armoury Crate silently when LumaBridge lets go.", true,
         true, "handback"},
        {"DualSense controller", "The lightbar, by USB or Bluetooth (experimental).", false, false, ""},
    };
    return k[static_cast<int>(c)];
}

// Whether the guide ticks a connection to start with. Everything is on, except:
//  - device connections for hardware the user said they don't have;
//  - a game SDK whose vendor's own runtime is in use (Synapse, Alienware Command Center):
//    LumaBridge's copy would replace it, so the vendor's devices would lose game lighting.
inline bool DefaultOn(Conn c, const Answers& a) {
    switch (c) {
    case Conn::Chroma: return !a.has(App::Synapse);
    case Conn::AlienFx: return !a.has(App::AlienwareCC);
    case Conn::LogitechDevices: return a.has(Brand::Logitech);
    case Conn::Azoth: return a.has(Brand::Azoth);
    case Conn::RamLighting: return a.has(Brand::FuryRam);
    case Conn::OpenRgb: return false;  // only if OpenRGB is on the PC (the guide checks)
    case Conn::DualSense: return a.has(Brand::DualSense);
    default: return true;
    }
}

// Why a connection starts unticked ("" when it doesn't).
inline const char* OffReason(Conn c, const Answers& a) {
    if (DefaultOn(c, a)) return "";
    switch (c) {
    case Conn::Chroma: return "Razer Synapse is installed: this would replace its Chroma runtime, and your Razer "
                              "devices would stop getting game lighting from Synapse.";
    case Conn::AlienFx: return "Alienware Command Center is installed: this would replace its AlienFX runtime.";
    case Conn::OpenRgb: return "OpenRGB isn't on this PC - LumaBridge doesn't need it.";
    default: return "Off because you said you don't have this hardware.";
    }
}

// "USB\VID_046D&PID_C547\..." -> 046D, C547. False if the ID has no VID / PID.
inline bool ParseVidPid(const std::wstring& id, uint16_t* vid, uint16_t* pid) {
    auto hex = [&](const wchar_t* key, uint16_t* out) {
        std::wstring up = id;
        for (auto& ch : up) ch = static_cast<wchar_t>(std::towupper(ch));
        const size_t at = up.find(key);
        if (at == std::wstring::npos || at + 8 > up.size()) return false;
        unsigned v = 0;
        for (size_t i = at + 4; i < at + 8; ++i) {
            const wchar_t ch = up[i];
            const int d = ch >= L'0' && ch <= L'9' ? ch - L'0' : ch >= L'A' && ch <= L'F' ? ch - L'A' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + static_cast<unsigned>(d);
        }
        *out = static_cast<uint16_t>(v);
        return true;
    };
    return hex(L"VID_", vid) && hex(L"PID_", pid);
}

// Hardware a USB device shows (by its vendor, and for ASUS the Azoth's product IDs).
inline void AddUsbDevice(Answers* a, uint16_t vid, uint16_t pid) {
    switch (vid) {
    case 0x046D: a->set(Brand::Logitech); break;
    case 0x1532: a->set(Brand::Razer); break;
    case 0x1038: a->set(Brand::SteelSeries); break;
    case 0x1B1C: a->set(Brand::Corsair); break;
    case 0x187C: a->set(Brand::Alienware); break;
    case 0x054C: a->set(Brand::DualSense); break;  // PS5 DualSense / DualSense Edge, USB or BT
    case 0x0B05:
        if (pid == 0x1A83 || pid == 0x1ACE) a->set(Brand::Azoth);  // cable, Omni receiver
        else a->set(Brand::Asus);  // the Aura controller and other ASUS gear
        break;
    default: break;
    }
}

}  // namespace luma::app::setup
