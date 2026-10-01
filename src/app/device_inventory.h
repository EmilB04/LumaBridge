// Windows device nodes and their categories. Separate from lighting support: being present
// never means a device has RGB or that LumaBridge can control it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace luma::app::inventory {

enum class Category {
    Input, Audio, Displays, Graphics, Components, Storage, Network, Bluetooth,
    Cameras, Printers, Cooling, Usb, System, Software, Other, Count
};

inline const char* Name(Category c) {
    static const char* names[] = {"Keyboards, mice and controllers", "Audio", "Monitors", "Graphics cards",
        "Processors, board and memory", "Storage", "Network", "Bluetooth", "Cameras", "Printers",
        "Cooling and power", "USB controllers and hubs", "System devices", "Software devices", "Other devices"};
    const int i = static_cast<int>(c);
    return i >= 0 && i < static_cast<int>(Category::Count) ? names[i] : "Other devices";
}

inline std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return s;
}

// Windows setup class names are invariant, unlike the localized device descriptions.
inline Category Classify(const std::string& setupClass) {
    const std::string c = Lower(setupClass);
    if (c == "keyboard" || c == "mouse" || c == "hidclass" || c == "xnacomposite" || c == "xboxcomposite")
        return Category::Input;
    if (c == "media" || c == "audioendpoint") return Category::Audio;
    if (c == "monitor") return Category::Displays;
    if (c == "display") return Category::Graphics;
    if (c == "processor" || c == "memory") return Category::Components;
    if (c == "diskdrive" || c == "cdrom" || c == "volume" || c == "scsiadapter" || c == "hdc" ||
        c == "storage" || c == "volumesnapshot") return Category::Storage;
    if (c == "net") return Category::Network;
    if (c == "bluetooth") return Category::Bluetooth;
    if (c == "camera" || c == "image") return Category::Cameras;
    if (c == "printer" || c == "printqueue" || c == "pnpprinters") return Category::Printers;
    if (c == "battery") return Category::Cooling;
    if (c == "usb" || c == "usbhub") return Category::Usb;
    if (c == "system" || c == "computer" || c == "firmware" || c == "securitydevices") return Category::System;
    if (c == "softwaredevice" || c == "softwarecomponent") return Category::Software;
    return Category::Other;
}

// What a device is, in plain English. Windows' own class descriptions follow the PC's display
// language, so a mixed-language list would result; the setup class names never change.
inline std::string KindName(const std::string& setupClass) {
    const std::string c = Lower(setupClass);
    struct Row { const char* cls; const char* name; };
    static const Row rows[] = {
        {"keyboard", "Keyboard"}, {"mouse", "Mouse"}, {"hidclass", "Input device (HID)"},
        {"xnacomposite", "Game controller"}, {"xboxcomposite", "Game controller"},
        {"media", "Sound device"}, {"audioendpoint", "Audio endpoint"}, {"monitor", "Monitor"},
        {"display", "Graphics adapter"}, {"processor", "Processor"}, {"memory", "Memory"},
        {"diskdrive", "Disk drive"}, {"cdrom", "Optical drive"}, {"volume", "Storage volume"},
        {"scsiadapter", "Storage controller"}, {"hdc", "Storage controller"}, {"storage", "Storage"},
        {"volumesnapshot", "Storage snapshot"}, {"net", "Network adapter"}, {"bluetooth", "Bluetooth"},
        {"camera", "Camera"}, {"image", "Imaging device"}, {"printer", "Printer"}, {"printqueue", "Print queue"},
        {"pnpprinters", "Printer port"}, {"battery", "Battery or power"}, {"usb", "USB controller or device"},
        {"usbhub", "USB hub"}, {"system", "System device"}, {"computer", "Computer"},
        {"firmware", "Firmware"}, {"securitydevices", "Security device"}, {"softwaredevice", "Software device"},
        {"softwarecomponent", "Software component"},
    };
    for (const Row& r : rows)
        if (c == r.cls) return r.name;
    return setupClass.empty() ? "Other device" : setupClass;
}

struct Device {
    std::string id, name, setupClass, className, manufacturer, location;
    Category category = Category::Other;
    uint16_t vid = 0, pid = 0;
    bool statusKnown = false;
    uint32_t problem = 0;
};

struct Scan {
    std::vector<Device> devices;
    unsigned long error = 0;  // Windows error if the inventory could not be completed
};

inline bool Matches(const Device& d, const std::string& query) {
    const std::string q = Lower(query);
    return q.empty() || Lower(d.name + " " + d.className + " " + d.setupClass + " " + d.manufacturer +
                              " " + d.location + " " + d.id + " " + Name(d.category)).find(q) != std::string::npos;
}

// Keep distinct instances, even when their product IDs or generic names are identical.
inline void Sort(std::vector<Device>& devices) {
    std::sort(devices.begin(), devices.end(), [](const Device& a, const Device& b) {
        if (a.category != b.category) return a.category < b.category;
        const auto an = Lower(a.name), bn = Lower(b.name);
        return an == bn ? a.id < b.id : an < bn;
    });
}

// Read-only scan of all present Windows device nodes, across all buses and classes.
Scan ScanPresentDevices();

}  // namespace luma::app::inventory
