// LibreHardwareMonitor's web server (Options > Remote Web Server > Run; http://127.0.0.1:8085
// /data.json) as a sensor source: fan speeds, CPU / board / GPU temperatures, voltages, ...
// It serves a tree (computer > hardware > [sub-hardware >] group > sensor); this flattens it.
// Pure parser, tested.
#pragma once

#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#include "json.h"

namespace luma::app::sensors {

enum class HardwareKind { Cpu, Gpu, Board, Memory, Storage, Other };
enum class SensorType { Temperature, Fan, Control, Load, Power, Voltage, Clock, Other };

struct Sensor {
    HardwareKind kind = HardwareKind::Other;
    std::string hardware;  // "AMD Ryzen 7 5800X", "Nuvoton NCT6798D"
    std::string name;      // "Core (Tctl/Tdie)", "CPU Fan"
    SensorType type = SensorType::Other;
    double value = 0;
    std::string unit;      // "°C", "RPM", "%", ...
};

namespace detail {

// "images_icon/mainboard.png" -> "mainboard".
inline std::string ImageStem(const std::string& image) {
    const size_t slash = image.find_last_of('/');
    std::string stem = image.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = stem.find('.');
    return dot == std::string::npos ? stem : stem.substr(0, dot);
}

// Whether a node's icon marks it as hardware, and which kind (group nodes such as
// "Temperatures" have icons too: temperature, fan, load, ...).
inline bool HardwareFromImage(const std::string& image, HardwareKind* kind) {
    const std::string s = ImageStem(image);
    if (s == "cpu") *kind = HardwareKind::Cpu;
    else if (s == "nvidia" || s == "ati" || s == "amd" || s == "intel" || s == "gpu") *kind = HardwareKind::Gpu;
    else if (s == "mainboard" || s == "chip") *kind = HardwareKind::Board;
    else if (s == "ram") *kind = HardwareKind::Memory;
    else if (s == "hdd" || s == "ssd" || s == "storage") *kind = HardwareKind::Storage;
    else if (s == "nic" || s == "battery" || s == "psu" || s == "controller") *kind = HardwareKind::Other;
    else return false;
    return true;
}

inline SensorType TypeFrom(const std::string& type, const std::string& group, const std::string& unit) {
    const std::string t = type.empty() ? group : type;
    if (t.rfind("Temperature", 0) == 0 || unit.find("\xC2\xB0") != std::string::npos) return SensorType::Temperature;
    if (t.rfind("Fan", 0) == 0 || unit == "RPM") return SensorType::Fan;
    if (t.rfind("Control", 0) == 0) return SensorType::Control;
    if (t.rfind("Load", 0) == 0) return SensorType::Load;
    if (t.rfind("Power", 0) == 0 || unit == "W") return SensorType::Power;
    if (t.rfind("Voltage", 0) == 0 || unit == "V") return SensorType::Voltage;
    if (t.rfind("Clock", 0) == 0 || unit == "MHz") return SensorType::Clock;
    return SensorType::Other;
}

// "45,5 °C" / "1200 RPM" -> 45.5 / 1200 and the unit. Accepts a decimal comma.
inline bool ParseValue(const std::string& text, double* value, std::string* unit) {
    std::string num;
    size_t i = 0;
    while (i < text.size() && text[i] == ' ') ++i;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+') num += c;
        else if (c == ',' || c == '.') num += '.';
        else break;
    }
    if (num.empty() || num == "-") return false;
    *value = std::atof(num.c_str());
    while (i < text.size() && text[i] == ' ') ++i;
    *unit = text.substr(i);
    return true;
}

inline void Walk(const Json& node, int depth, HardwareKind kind, const std::string& hardware,
                 const std::string& group, std::vector<Sensor>* out) {
    const std::string text = node["Text"].String();
    const std::string image = node["ImageURL"].String();
    const Json& children = node["Children"];
    const bool leaf = children.size() == 0;
    if (leaf) {
        Sensor s;
        if (depth < 3 || !ParseValue(node["Value"].String(), &s.value, &s.unit)) return;
        s.kind = kind;
        s.hardware = hardware;
        s.name = text;
        s.type = TypeFrom(node["Type"].String(), group, s.unit);
        out->push_back(s);
        return;
    }
    HardwareKind k = kind;
    std::string hw = hardware, grp = group;
    HardwareKind fromImage;
    if (depth >= 2 && HardwareFromImage(image, &fromImage)) {
        hw = text;  // a hardware node (sub-hardware like the Super I/O chip counts too)
        if (fromImage != HardwareKind::Other || k == HardwareKind::Other) k = fromImage;
    } else if (depth >= 3) {
        grp = text;  // "Temperatures", "Fans", ...
    }
    for (size_t i = 0; i < children.size(); ++i) Walk(children[i], depth + 1, k, hw, grp, out);
}

}  // namespace detail

// The best sensor of a kind/type: the first whose name contains one of `preferred` (in
// order), else the first of that kind/type. nullptr if none.
inline const Sensor* PickSensor(const std::vector<Sensor>& all, HardwareKind kind, SensorType type,
                                std::initializer_list<const char*> preferred) {
    for (const char* want : preferred)
        for (const Sensor& s : all)
            if (s.kind == kind && s.type == type && s.name.find(want) != std::string::npos) return &s;
    for (const Sensor& s : all)
        if (s.kind == kind && s.type == type) return &s;
    return nullptr;
}

inline std::vector<Sensor> ParseLhm(const Json& root) {
    std::vector<Sensor> out;
    detail::Walk(root, 0, HardwareKind::Other, "", "", &out);
    return out;
}

}  // namespace luma::app::sensors
