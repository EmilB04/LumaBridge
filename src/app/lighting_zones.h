// Lighting zones: groups of devices the user puts together (RAM and fans; keyboard and mouse;
// the controller), each with a lighting of its own and its own mode, manual or auto. Devices
// in no zone make up "Everything", which uses the main lighting and the mode in the
// header. Pure and tested.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "device_lighting.h"

namespace luma::app {

// What an auto zone shows between games (a game's lighting always wins while one is running).
enum class ZoneIdle { Look, Rainbow, Off };

struct Zone {
    std::string id;    // stable ("z1", "z2", ...); the name can change
    std::string name;
    std::vector<std::string> devices;  // device::k* ids (a device is in one zone at most)
    bool manual = true;                // manual: its look always; auto: games, then `idle`
    ZoneIdle idle = ZoneIdle::Look;
    Look look;                         // the zone's own lighting (manual, or auto between games)
    // An effect across the zone's devices, each fan / stick / device a part (fx::Kind setup
    // effects); 0: none.
    int setupEffect = 0;
    Rgb setupColor1{0, 140, 255}, setupColor2{0, 0, 0};
    float setupSpeed = 2.f;
    int order = 1;          // FlowDirection (1..4): the order of the parts across the zone
    // An ordinary moving effect travelling across the zone's devices (FlowDirection; 0: off).
    int flowDirection = 0;
    float flowSeconds = 1.5f;
    float brightness = 1;   // on top of the overall one
    bool reverse = false;   // moving effects run the other way

    bool Has(const std::string& device) const {
        return std::find(devices.begin(), devices.end(), device) != devices.end();
    }
    bool operator==(const Zone& o) const {
        return id == o.id && name == o.name && devices == o.devices && manual == o.manual && idle == o.idle &&
               look == o.look && setupEffect == o.setupEffect && setupColor1 == o.setupColor1 &&
               setupColor2 == o.setupColor2 && setupSpeed == o.setupSpeed && order == o.order &&
               flowDirection == o.flowDirection && flowSeconds == o.flowSeconds && brightness == o.brightness &&
               reverse == o.reverse;
    }
};

constexpr size_t kMaxZones = 8;

// A device's zone, or nullptr (it's in Everything).
inline const Zone* ZoneOf(const std::vector<Zone>& zones, const std::string& device) {
    for (const Zone& z : zones)
        if (z.Has(device)) return &z;
    return nullptr;
}
inline Zone* ZoneOf(std::vector<Zone>& zones, const std::string& device) {
    for (Zone& z : zones)
        if (z.Has(device)) return &z;
    return nullptr;
}

// The device a lighting id belongs to for zones: a Nollie channel or fan is part of the Nollie
// fans.
inline std::string ZoneDevice(const std::string& lightingId) {
    return lightingId.rfind("nollie.", 0) == 0 ? std::string(device::kNollie) : lightingId;
}

inline const Zone* FindZone(const std::vector<Zone>& zones, const std::string& id) {
    for (const Zone& z : zones)
        if (z.id == id) return &z;
    return nullptr;
}
inline Zone* FindZone(std::vector<Zone>& zones, const std::string& id) {
    for (Zone& z : zones)
        if (z.id == id) return &z;
    return nullptr;
}

// Auto zones can need game capture even while Everything is manual.
inline bool NeedsGameCapture(bool everythingAuto, const std::vector<Zone>& zones) {
    return everythingAuto || std::any_of(zones.begin(), zones.end(), [](const Zone& z) { return !z.manual && !z.devices.empty(); });
}
inline std::vector<std::string> DevicesOutsideZones(const std::vector<Zone>& zones) {
    std::vector<std::string> out;
    for (const char* id : device::All()) if (!ZoneOf(zones, id)) out.emplace_back(id);
    return out;
}

// Removing a zone returns its devices to Everything (membership lives only in the zone).
inline bool DeleteZone(std::vector<Zone>& zones, const std::string& id) {
    if (id.empty()) return false;
    const size_t before = zones.size();
    zones.erase(std::remove_if(zones.begin(), zones.end(), [&](const Zone& z) { return z.id == id; }), zones.end());
    return zones.size() != before;
}

// A new id nobody uses.
inline std::string NewZoneId(const std::vector<Zone>& zones) {
    for (int n = 1;; ++n) {
        const std::string id = "z" + std::to_string(n);
        if (!FindZone(zones, id)) return id;
    }
}

// Puts `device` into zone `zoneId` ("" : Everything), taking it out of the zone it was in.
inline void MoveDevice(std::vector<Zone>& zones, const std::string& device, const std::string& zoneId) {
    for (Zone& z : zones) z.devices.erase(std::remove(z.devices.begin(), z.devices.end(), device), z.devices.end());
    if (Zone* z = FindZone(zones, zoneId)) z->devices.push_back(device);
}

// Zones keep only known devices, each in one zone only; names are never empty.
inline void Normalize(std::vector<Zone>& zones) {
    std::vector<std::string> seen;
    for (Zone& z : zones) {
        std::vector<std::string> kept;
        for (const std::string& d : z.devices) {
            if (std::find(seen.begin(), seen.end(), d) != seen.end()) continue;
            bool known = false;
            for (const char* k : device::All()) known = known || d == k;
            if (!known) continue;
            kept.push_back(d);
            seen.push_back(d);
        }
        z.devices = kept;
        if (z.name.empty()) z.name = "Zone";
    }
    if (zones.size() > kMaxZones) zones.resize(kMaxZones);
}

// Devices that had a lighting of their own (before zones) become zones of their own, so
// nothing changes for them: their look, and brightness / direction stay on the device.
// Returns the zones made.
inline std::vector<Zone> ZonesFromOwnLooks(std::map<std::string, DeviceLighting>& lighting, const std::vector<Zone>& existing) {
    std::vector<Zone> made, all = existing;
    for (const char* id : device::All()) {
        auto it = lighting.find(id);
        if (it == lighting.end() || !it->second.own || it->second.native || ZoneOf(all, id)) continue;
        if (all.size() >= kMaxZones) break;
        Zone z;
        z.id = NewZoneId(all);
        z.name = device::Name(id);
        z.devices = {id};
        z.look = it->second.look;
        it->second.own = false;  // now the zone's
        all.push_back(z);
        made.push_back(z);
    }
    return made;
}

// ---- Saved form -----------------------------------------------------------------------------

// "id|name|manual|idle|setupEffect|setupSpeed|order|flowDirection|flowSeconds|brightness|reverse|
//  RRGGBB|RRGGBB|device,device|<look>" ("|" can't be in the name).
inline std::string Encode(const Zone& z) {
    DeviceLighting d;
    d.own = true;
    d.look = z.look;
    std::string name = z.name, devices;
    for (char& c : name)
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    for (const std::string& dev : z.devices) devices += (devices.empty() ? "" : ",") + dev;
    auto hex = [](Rgb c) {
        static const char* k = "0123456789ABCDEF";
        std::string s;
        for (uint8_t v : {c.r, c.g, c.b}) {
            s += k[v >> 4];
            s += k[v & 15];
        }
        return s;
    };
    char buf[64];
    snprintf(buf, sizeof buf, "%d|%g|%d|%d|%g|%g|%d", z.setupEffect, z.setupSpeed, z.order, z.flowDirection, z.flowSeconds,
             z.brightness, z.reverse ? 1 : 0);
    return z.id + "|" + name + "|" + (z.manual ? "1" : "0") + "|" + std::to_string(static_cast<int>(z.idle)) + "|" + buf +
           "|" + hex(z.setupColor1) + "|" + hex(z.setupColor2) + "|" + devices + "|" + EncodeDevice(d);
}

inline bool Decode(const std::string& s, Zone* out) {
    std::vector<std::string> f;
    size_t pos = 0;
    for (int i = 0; i < 14; ++i) {  // 14 fields before the look
        const size_t bar = s.find('|', pos);
        if (bar == std::string::npos) return false;
        f.push_back(s.substr(pos, bar - pos));
        pos = bar + 1;
    }
    auto hex = [](const std::string& h, Rgb* c) {
        if (h.size() != 6) return false;
        char* end = nullptr;
        const unsigned long v = std::strtoul(h.c_str(), &end, 16);
        if (*end) return false;
        *c = Rgb{static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
        return true;
    };
    DeviceLighting d;
    if (!DecodeDevice(s.substr(pos), &d) || f[0].empty()) return false;
    Zone z;
    z.id = f[0];
    z.name = f[1];
    z.manual = f[2] != "0";
    const int idle = settings::Integer(f[3]);
    z.idle = idle >= 0 && idle <= 2 ? static_cast<ZoneIdle>(idle) : ZoneIdle::Look;
    const int effect = settings::Integer(f[4]);
    z.setupEffect = effect >= fx::kFirstSetupEffect && effect < fx::kFirstSetupEffect + fx::kSetupEffects ? effect : 0;
    z.setupSpeed = settings::Clamped(f[5], z.setupSpeed, 0.1f, 10.f);
    z.order = std::clamp(settings::Integer(f[6]), 1, 4);
    z.flowDirection = std::clamp(settings::Integer(f[7]), 0, 4);
    z.flowSeconds = settings::Clamped(f[8], z.flowSeconds, 0.2f, 5.f);
    z.brightness = settings::Clamped(f[9], z.brightness, 0.f, 1.f);
    z.reverse = f[10] == "1";
    if (!hex(f[11], &z.setupColor1) || !hex(f[12], &z.setupColor2)) return false;
    for (size_t at = 0; at <= f[13].size();) {
        const size_t comma = f[13].find(',', at);
        const std::string dev = f[13].substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!dev.empty()) z.devices.push_back(dev);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    z.look = d.look;
    *out = z;
    return true;
}

}  // namespace luma::app
