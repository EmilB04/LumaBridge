// EA SPORTS WRC lighting from the game's own UDP telemetry. The game sends packets laid out by
// structure files in Documents\My Games\WRC\telemetry\udp, picked in telemetry\config.json
// (EA's "UDP Telemetry Guide"). LumaBridge adds its own small structure, lumabridge.json, and one
// entry in config.json, so each packet during a stage is just (tightly packed):
//   u8 vehicle_gear_index, f32 vehicle_engine_rpm_current, f32 vehicle_engine_rpm_max
// The game only sends "session_update" packets on a stage, not in the menus. The config.json
// edits are pure text functions here, so they're tested; not checked against the running game.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "effects.h"
#include "rev_lights.h"

namespace luma::app::games {

class WrcLighting {
public:
    static constexpr uint16_t kDefaultPort = 49718;
    static constexpr uint64_t kStaleMs = 1000;
    static constexpr size_t kSize = 9;

    bool OnPacket(const uint8_t* p, size_t n, uint64_t now) {
        if (n != kSize) return false;  // exactly LumaBridge's structure
        float rpm, maxRpm;
        std::memcpy(&rpm, p + 1, 4);
        std::memcpy(&maxRpm, p + 5, 4);
        if (!(maxRpm > 500 && maxRpm < 30000) || !(rpm >= 0 && rpm < 30000)) return false;
        lastSeen_ = now;
        rpm_ = rpm;
        maxRpm_ = maxRpm;
        return true;
    }

    // On a stage with the engine running.
    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && rpm_ > 50; }

    fx::Params Current() const { return RevLights(Clamp01(rpm_ / maxRpm_), false); }

private:
    uint64_t lastSeen_ = 0;
    double rpm_ = 0, maxRpm_ = 1;
};

// The structure file LumaBridge adds to telemetry\udp (lumabridge.json).
inline std::string WrcStructureText() {
    return "{\n"
           "\t\"versions\": { \"schema\": 1, \"data\": 3 },\n"
           "\t\"id\": \"lumabridge\",\n"
           "\t\"header\": { \"channels\": [] },\n"
           "\t\"packets\": [\n"
           "\t\t{\n"
           "\t\t\t\"id\": \"session_update\",\n"
           "\t\t\t\"channels\": [ \"vehicle_gear_index\", \"vehicle_engine_rpm_current\", \"vehicle_engine_rpm_max\" ]\n"
           "\t\t}\n"
           "\t]\n"
           "}\n";
}

// Whether config.json already sends LumaBridge's packets.
inline bool WrcConfigHasLumaBridge(const std::string& text) {
    return text.find("\"lumabridge\"") != std::string::npos;
}

// The port LumaBridge's entry in config.json sends to, 0 without one.
inline int WrcConfigPortIn(const std::string& text) {
    const size_t mine = text.find("\"lumabridge\"");
    if (mine == std::string::npos) return 0;
    const size_t end = text.find('}', mine), key = text.find("\"port\"", mine);
    if (key == std::string::npos || key > end) return 0;
    size_t at = text.find(':', key);
    if (at == std::string::npos) return 0;
    ++at;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
    int port = 0;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9' && port < 100000) port = port * 10 + (text[at++] - '0');
    return port;
}

// config.json's text with LumaBridge's entry added to udp.packets (sending to 127.0.0.1:port),
// or with `remove`, taken out again. "" when the text has no udp packets list to change.
inline std::string WrcConfigText(const std::string& text, int port, bool remove) {
    const size_t udp = text.find("\"udp\"");
    if (udp == std::string::npos) return "";
    const size_t packets = text.find("\"packets\"", udp);
    if (packets == std::string::npos) return "";
    const size_t open = text.find('[', packets);
    if (open == std::string::npos) return "";
    const size_t mine = text.find("\"lumabridge\"");
    if (remove) {
        if (mine == std::string::npos) return text;
        // The object holding it, and the comma that joins it to the next (or the previous) one.
        size_t from = text.rfind('{', mine), to = text.find('}', mine);
        if (from == std::string::npos || to == std::string::npos || from < open) return "";
        ++to;
        size_t after = to;
        while (after < text.size() && (text[after] == ' ' || text[after] == '\t' || text[after] == '\r' || text[after] == '\n'))
            ++after;
        if (after < text.size() && text[after] == ',') {
            to = after + 1;
        } else {
            size_t before = from;
            while (before > open && (text[before - 1] == ' ' || text[before - 1] == '\t' || text[before - 1] == '\r' ||
                                     text[before - 1] == '\n'))
                --before;
            if (before > open && text[before - 1] == ',') from = before - 1;
        }
        // Take the line's own indent and line break with it.
        while (from > open && (text[from - 1] == ' ' || text[from - 1] == '\t')) --from;
        if (from > open && text[from - 1] == '\n') --from;
        if (from > open && text[from - 1] == '\r') --from;
        return text.substr(0, from) + text.substr(to);
    }
    if (mine != std::string::npos) return text;
    size_t next = open + 1;
    while (next < text.size() && (text[next] == ' ' || text[next] == '\t' || text[next] == '\r' || text[next] == '\n')) ++next;
    const bool empty = next < text.size() && text[next] == ']';
    const std::string entry = "\n\t\t\t{\n"
                              "\t\t\t\t\"structure\": \"lumabridge\",\n"
                              "\t\t\t\t\"packet\": \"session_update\",\n"
                              "\t\t\t\t\"ip\": \"127.0.0.1\",\n"
                              "\t\t\t\t\"port\": " + std::to_string(port) + ",\n"
                              "\t\t\t\t\"frequencyHz\": 60,\n"
                              "\t\t\t\t\"bEnabled\": true\n"
                              "\t\t\t}" + std::string(empty ? "" : ",");
    return text.substr(0, open + 1) + entry + text.substr(open + 1);
}

}  // namespace luma::app::games
