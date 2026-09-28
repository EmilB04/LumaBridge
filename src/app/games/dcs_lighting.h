// DCS World lighting from DCS's own export interface: Saved Games\DCS\Scripts\Export.lua is
// DCS's documented place for scripts that read the aircraft's state (SRS, Tacview and
// Helios use it too). LumaBridge adds one line there that loads its own LumaBridge.lua,
// which sends a few values as JSON over UDP to 127.0.0.1:kPort, 20 times a second. It only
// reads; servers that forbid exporting your own aircraft simply leave it quiet. Pure C++,
// tested.
//
// What shows, most important first:
//   master warning (red strobe) > high G (red, deeper as the G climbs) and negative G >
//   gear down in the air (three greens) > the sky: blue by day, orange at dawn and dusk, deep
//   blue at night, dimmed on the ground with the engines off.
#pragma once

#include <cstdint>
#include <string>

#include "effects.h"
#include "json.h"

namespace luma::app::games {

class DcsLighting {
public:
    static constexpr int kPort = 49717;
    static constexpr uint64_t kStaleMs = 1500;

    // One datagram from LumaBridge.lua. False if it isn't one.
    bool OnPacket(const Json& j, uint64_t now) {
        if (!j.IsObject()) return false;
        if (j["stop"].Number(0) != 0) {  // the mission ended
            lastSeen_ = 0;
            return true;
        }
        lastSeen_ = now;
        alive_ = j["alive"].Number(0) != 0;
        g_ = j["g"].Number(1);
        gear_ = j["gear"].Number(0);
        agl_ = j["agl"].Number(0);
        rpm_ = j["rpm"].Number(0);
        warning_ = j["mw"].Number(0) != 0;
        timeOfDay_ = j["tod"].Number(43200);
        return true;
    }

    // Flying (or sitting in) an aircraft, and sending.
    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && alive_; }

    fx::Params Current() const {
        using fx::Kind;
        if (warning_) return Make(Kind::Strobe, {255, 0, 0}, 5);
        if (g_ >= 6) return Make(Kind::Static, Scale({255, 0, 0}, g_ >= 9 ? 1.0 : 0.45 + (g_ - 6) / 3 * 0.55));
        if (g_ <= -1.5) return Make(Kind::Static, {255, 0, 90});
        const bool airborne = agl_ > 3;
        if (airborne && gear_ > 0.9) return Make(Kind::Static, {0, 255, 60});
        Rgb sky = Sky(timeOfDay_);
        if (!airborne && rpm_ < 1) sky = Scale(sky, 0.35);
        return Make(Kind::Static, sky);
    }

private:
    // By the mission's time of day (seconds since midnight, any day).
    static Rgb Sky(double seconds) {
        const double h = static_cast<double>(static_cast<int64_t>(seconds) % 86400) / 3600.0;
        if ((h >= 5.5 && h < 7.5) || (h >= 18 && h < 20)) return {255, 110, 30};
        if (h >= 7.5 && h < 18) return {60, 150, 255};
        return {20, 30, 110};
    }
    static fx::Params Make(fx::Kind k, Rgb c, double speed = 0) {
        fx::Params p;
        p.kind = k;
        p.color1 = c;
        p.speed = speed;
        return p;
    }

    uint64_t lastSeen_ = 0;
    bool alive_ = false, warning_ = false;
    double g_ = 1, gear_ = 0, agl_ = 0, rpm_ = 0, timeOfDay_ = 43200;
};

// ---- Setup -------------------------------------------------------------------------------

// The line LumaBridge adds to Export.lua (the same pattern SRS uses).
inline const char* DcsExportHook() {
    return "local lumaLfs=require('lfs'); dofile(lumaLfs.writedir()..[[Scripts\\LumaBridge.lua]])";
}

// Export.lua's text with LumaBridge's line added at the end (unchanged if already there).
inline std::string DcsExportWithHook(const std::string& text) {
    if (text.find(DcsExportHook()) != std::string::npos) return text;
    std::string out = text;
    if (!out.empty() && out.back() != '\n') out += "\r\n";
    return out + DcsExportHook() + "\r\n";
}

// ... and with it taken out again (everything else untouched).
inline std::string DcsExportWithoutHook(const std::string& text) {
    std::string out = text;
    const std::string hook = DcsExportHook();
    for (size_t at; (at = out.find(hook)) != std::string::npos;) {
        size_t end = at + hook.size();
        if (end < out.size() && out[end] == '\r') ++end;
        if (end < out.size() && out[end] == '\n') ++end;
        out.erase(at, end - at);
    }
    return out;
}

inline bool DcsHasHook(const std::string& text) { return text.find(DcsExportHook()) != std::string::npos; }

// LumaBridge.lua: reads the aircraft through DCS's export functions and sends it to LumaBridge.
inline std::string DcsScriptText() {
    return "-- LumaBridge: sends your aircraft's state to LumaBridge on this PC (127.0.0.1:" +
           std::to_string(DcsLighting::kPort) +
           ") for your RGB lighting.\r\n"
           "-- It only reads what DCS's export functions report. Written by LumaBridge; remove it there.\r\n"
           "pcall(function()\r\n"
           "  package.path = package.path .. \";.\\\\LuaSocket\\\\?.lua\"\r\n"
           "  package.cpath = package.cpath .. \";.\\\\LuaSocket\\\\?.dll\"\r\n"
           "  local socket = require(\"socket\")\r\n"
           "  local udp = socket.udp()\r\n"
           "  udp:settimeout(0)\r\n"
           "  udp:setpeername(\"127.0.0.1\", " +
           std::to_string(DcsLighting::kPort) +
           ")\r\n"
           "  local nextSend = 0\r\n"
           "  local prevAfter, prevStop = LuaExportAfterNextFrame, LuaExportStop\r\n"
           "  local function num(v) if type(v) == \"number\" then return v end return 0 end\r\n"
           "  local function get(f) if not f then return nil end local ok, v = pcall(f) if ok then return v end end\r\n"
           "  function LuaExportAfterNextFrame()\r\n"
           "    if prevAfter then pcall(prevAfter) end\r\n"
           "    local t = num(get(LoGetModelTime))\r\n"
           "    if t < nextSend then return end\r\n"
           "    nextSend = t + 0.05\r\n"
           "    pcall(function()\r\n"
           "      local me = get(LoGetSelfData)\r\n"
           "      local eng = get(LoGetEngineInfo)\r\n"
           "      local mech = get(LoGetMechInfo)\r\n"
           "      local mcp = get(LoGetMCPState)\r\n"
           "      local acc = get(LoGetAccelerationUnits)\r\n"
           "      local rpm = 0\r\n"
           "      if eng and eng.RPM then rpm = math.max(num(eng.RPM.left), num(eng.RPM.right)) end\r\n"
           "      local gear = 0\r\n"
           "      if mech and mech.gear then gear = num(mech.gear.value) end\r\n"
           "      local mw = 0\r\n"
           "      if mcp and mcp.MasterWarning then mw = 1 end\r\n"
           "      udp:send(string.format('{\"alive\":%d,\"tod\":%.0f,\"g\":%.2f,\"agl\":%.1f,\"gear\":%.2f,"
           "\"rpm\":%.1f,\"mw\":%d}',\r\n"
           "        me and 1 or 0, num(get(LoGetMissionStartTime)) + t, acc and num(acc.y) or 1,\r\n"
           "        num(get(LoGetAltitudeAboveGroundLevel)), gear, rpm, mw))\r\n"
           "    end)\r\n"
           "  end\r\n"
           "  function LuaExportStop()\r\n"
           "    if prevStop then pcall(prevStop) end\r\n"
           "    pcall(function() udp:send('{\"stop\":1}') udp:close() end)\r\n"
           "  end\r\n"
           "end)\r\n";
}

}  // namespace luma::app::games
