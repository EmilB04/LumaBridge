// Human names for hardware strings: "ASUSTeK COMPUTER INC." + "ROG STRIX B550-F GAMING" ->
// "ASUS ROG STRIX B550-F GAMING", memory part numbers -> product lines ("HyperX FURY"), and
// CPU / GPU names without the (R)/(TM) noise. Pure C++, tested.
#pragma once

#include <string>

namespace luma::app::sensors {

namespace detail {
inline void Erase(std::string* s, const std::string& what) {
    for (size_t p; (p = s->find(what)) != std::string::npos;) s->erase(p, what.size());
}
inline std::string Tidy(std::string s) {
    std::string out;
    for (char c : s)
        if (!(c == ' ' && (out.empty() || out.back() == ' '))) out += c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
inline bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
}  // namespace detail

inline std::string FriendlyMaker(const std::string& maker) {
    static const struct {
        const char* contains;
        const char* name;
    } kMakers[] = {{"ASUSTeK", "ASUS"}, {"Micro-Star", "MSI"}, {"Gigabyte", "Gigabyte"}, {"GIGABYTE", "Gigabyte"},
                   {"ASRock", "ASRock"}, {"Kingston", "Kingston"}, {"Corsair", "Corsair"}, {"G Skill", "G.SKILL"},
                   {"G.Skill", "G.SKILL"}, {"Micron", "Crucial"}, {"Samsung", "Samsung"}, {"SK Hynix", "SK hynix"},
                   {"Hynix", "SK hynix"}};
    for (const auto& m : kMakers)
        if (maker.find(m.contains) != std::string::npos) return m.name;
    return detail::Tidy(maker);
}

inline std::string FriendlyBoard(const std::string& maker, const std::string& product) {
    const std::string m = FriendlyMaker(maker);
    if (product.empty()) return m;
    if (m.empty() || product.find(m) == 0) return detail::Tidy(product);
    return m + " " + detail::Tidy(product);
}

// Memory product line from the part number, e.g. "HX436C17FB3/8" -> "HyperX FURY",
// "KF3600C17D4/8GX" -> "Kingston FURY". Falls back to maker + part number.
inline std::string FriendlyMemory(const std::string& maker, const std::string& part) {
    using detail::StartsWith;
    const std::string p = detail::Tidy(part);
    if (StartsWith(p, "HX4") || StartsWith(p, "HX3") || StartsWith(p, "HX2")) {
        if (p.find("FB") != std::string::npos || p.find("FR") != std::string::npos) return "HyperX FURY";
        if (p.find("PB") != std::string::npos) return "HyperX Predator";
        return "HyperX";
    }
    if (StartsWith(p, "KF")) return "Kingston FURY";
    if (StartsWith(p, "CMW")) return "Corsair Vengeance RGB";
    if (StartsWith(p, "CMH") || StartsWith(p, "CMN")) return "Corsair Vengeance RGB";
    if (StartsWith(p, "CMK")) return "Corsair Vengeance";
    if (StartsWith(p, "CMT")) return "Corsair Dominator";
    if (StartsWith(p, "F4-") || StartsWith(p, "F5-")) return "G.SKILL";
    if (StartsWith(p, "BL")) return "Crucial Ballistix";
    const std::string m = FriendlyMaker(maker);
    return detail::Tidy(m.empty() ? p : p.empty() ? m : m + " " + p);
}

// "AMD Ryzen 7 5800X 8-Core Processor" -> "AMD Ryzen 7 5800X",
// "Intel(R) Core(TM) i7-12700K CPU @ 3.60GHz" -> "Intel Core i7-12700K".
inline std::string FriendlyCpu(std::string name) {
    detail::Erase(&name, "(R)");
    detail::Erase(&name, "(TM)");
    detail::Erase(&name, "(tm)");
    for (const char* tail : {" CPU @", " @ ", "-Core Processor", " Processor"}) {
        const size_t p = name.find(tail);
        if (p == std::string::npos) continue;
        size_t cut = p;
        if (std::string(tail) == "-Core Processor") {  // drop "8-Core Processor" including the count
            while (cut > 0 && name[cut - 1] != ' ') --cut;
        }
        name.erase(cut);
    }
    return detail::Tidy(name);
}

inline std::string FriendlyGpu(std::string name) {
    detail::Erase(&name, "(R)");
    detail::Erase(&name, "(TM)");
    return detail::Tidy(name);
}

}  // namespace luma::app::sensors
