// Recognising RGB hardware LumaBridge doesn't light itself: USB vendors of RGB brands, RGB
// memory by its part number, and AIO coolers by their pump's USB ID, so the setup guide and the Devices page can name what's in
// a PC and say how it can be lit (natively, through OpenRGB, or with the brand's own app).
// Pure, tested.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace luma::app::catalog {

// USB devices of RGB brands: a whole vendor (pid 0) where that vendor makes RGB gear almost
// only, else single products. Vendors whose IDs also cover everyday devices (USB sticks,
// printers, card readers, generic chips) are left out, so nothing is named wrongly.
struct UsbEntry {
    uint16_t vid, pid;  // pid 0: any product of the vendor
    const char* brand;
};
inline const std::vector<UsbEntry>& RgbUsb() {
    static const std::vector<UsbEntry> k{
        {0x046D, 0, "Logitech"},      {0x1532, 0, "Razer"},          {0x1038, 0, "SteelSeries"},
        {0x1B1C, 0, "Corsair"},       {0x0B05, 0, "ASUS"},           {0x187C, 0, "Alienware"},
        {0x1E71, 0, "NZXT"},          {0x2516, 0, "Cooler Master"},  {0x264A, 0, "Thermaltake"},
        {0x1462, 0, "MSI"},           {0x3434, 0, "Keychron"},
        {0x048D, 0x5702, "Gigabyte RGB Fusion"}, {0x048D, 0x8297, "Gigabyte RGB Fusion"},
    };
    return k;
}

inline const char* RgbBrand(uint16_t vid, uint16_t pid) {
    for (const UsbEntry& e : RgbUsb())
        if (e.vid == vid && (e.pid == 0 || e.pid == pid)) return e.brand;
    return nullptr;
}

// Brands found among USB (vendor, product) pairs, each once, in the catalogue's order.
// `native`: vendors LumaBridge lights itself, left out.
inline std::vector<std::string> RgbBrands(const std::vector<std::pair<uint16_t, uint16_t>>& usb,
                                          const std::vector<uint16_t>& native = {}) {
    std::vector<std::string> out;
    for (const UsbEntry& e : RgbUsb()) {
        bool skip = false;
        for (uint16_t n : native) skip = skip || n == e.vid;
        for (const std::string& b : out) skip = skip || b == e.brand;
        if (skip) continue;
        for (const auto& [vid, pid] : usb)
            if (vid == e.vid && (e.pid == 0 || e.pid == pid)) {
                out.push_back(e.brand);
                break;
            }
    }
    return out;
}

namespace detail {
inline std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    return s;
}
}  // namespace detail

// An RGB memory line from a module's SMBIOS maker and part number ("" when the part number
// doesn't say RGB). Kingston FURY / HyperX RGB are recognised in setup_hardware.h.
//   Corsair: CMW Vengeance RGB (Pro), CMN Vengeance RGB RT, CMH Vengeance RGB (DDR5),
//            CMT Dominator Platinum RGB.
//   G.Skill: ...TZR Trident Z RGB, ...TZN Trident Z Neo, ...TZ5R / TZ5N Trident Z5 RGB / Neo,
//            ...GTR Trident Z Royal.
inline std::string RgbMemory(const std::string& maker, const std::string& part) {
    const std::string m = detail::Upper(maker), p = detail::Upper(part);
    if (m.find("CORSAIR") != std::string::npos || p.rfind("CM", 0) == 0) {
        if (p.rfind("CMW", 0) == 0) return "Corsair Vengeance RGB";
        if (p.rfind("CMN", 0) == 0) return "Corsair Vengeance RGB RT";
        if (p.rfind("CMH", 0) == 0) return "Corsair Vengeance RGB";
        if (p.rfind("CMT", 0) == 0) return "Corsair Dominator Platinum RGB";
    }
    if (m.find("G.SKILL") != std::string::npos || m.find("GSKILL") != std::string::npos || p.rfind("F4-", 0) == 0 ||
        p.rfind("F5-", 0) == 0) {
        if (p.find("TZ5R") != std::string::npos) return "G.Skill Trident Z5 RGB";
        if (p.find("TZ5N") != std::string::npos) return "G.Skill Trident Z5 Neo";
        if (p.find("TZR") != std::string::npos) return "G.Skill Trident Z RGB";
        if (p.find("TZN") != std::string::npos) return "G.Skill Trident Z Neo";
        if (p.find("GTR") != std::string::npos) return "G.Skill Trident Z Royal";
    }
    return "";
}

// ---- AIO water coolers ----------------------------------------------------------------
// The pump of an all-in-one cooler is a USB device, so it can be named: by product ID, as
// listed by liquidctl (the open-source cooler tool). One ID often covers every radiator size
// of a model line, so the names are the line's.

struct AioModel {
    uint16_t vid, pid;
    const char* name;
    bool lcd;       // a screen on the pump head
    bool pumpRgb;   // a lit pump head (lit by the brand's own app)
};

inline const std::vector<AioModel>& AioModels() {
    static const std::vector<AioModel> k{
        {0x1E71, 0x170E, "NZXT Kraken X42/X52/X62/X72", false, true},
        {0x1E71, 0x1715, "NZXT Kraken M22", false, true},
        {0x1E71, 0x2007, "NZXT Kraken X53/X63/X73", false, true},
        {0x1E71, 0x2014, "NZXT Kraken X53/X63/X73", false, true},
        {0x1E71, 0x3008, "NZXT Kraken Z53/Z63/Z73", true, false},
        {0x1E71, 0x300C, "NZXT Kraken Elite", true, false},
        {0x1E71, 0x300E, "NZXT Kraken", true, false},
        {0x1E71, 0x3012, "NZXT Kraken Elite RGB", true, true},
        {0x1E71, 0x3014, "NZXT Kraken Plus", true, true},
        {0x1B1C, 0x0C12, "Corsair Hydro H150i Pro", false, true},
        {0x1B1C, 0x0C13, "Corsair Hydro H115i Pro", false, true},
        {0x1B1C, 0x0C15, "Corsair Hydro H100i Pro", false, true},
        {0x1B1C, 0x0C17, "Corsair H115i Platinum", false, true},
        {0x1B1C, 0x0C18, "Corsair H100i Platinum", false, true},
        {0x1B1C, 0x0C19, "Corsair H100i Platinum SE", false, true},
        {0x1B1C, 0x0C20, "Corsair H100i Pro XT", false, true},
        {0x1B1C, 0x0C21, "Corsair H115i Pro XT", false, true},
        {0x1B1C, 0x0C22, "Corsair H150i Pro XT", false, true},
        {0x0B05, 0x1988, "ASUS ROG Ryujin II", true, true},
    };
    return k;
}

// The AIO cooler among USB (vendor, product) pairs, or nullptr. NZXT numbers its screen
// Krakens 0x30xx, so a newer one than listed is still recognised as one.
inline const AioModel* FindAio(const std::vector<std::pair<uint16_t, uint16_t>>& usb) {
    for (const auto& [vid, pid] : usb)
        for (const AioModel& m : AioModels())
            if (m.vid == vid && m.pid == pid) return &m;
    static thread_local AioModel newer{};
    for (const auto& [vid, pid] : usb)
        if (vid == 0x1E71 && pid >= 0x3000 && pid <= 0x30FF) {
            newer = {vid, pid, "NZXT Kraken", true, true};
            return &newer;
        }
    return nullptr;
}

}  // namespace luma::app::catalog
