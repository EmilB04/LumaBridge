// Microsoft Flight Simulator (2020 / 2024) lighting from SimConnect, the sim's own interface
// for add-ons: LumaBridge asks it for a few values about your aircraft many times a second.
// Pure C++, tested; the SimConnect side is in game_feeds.cpp.
//
// What shows, most important first:
//   crashed (slow red breathing) > stall warning (fast red strobe) > overspeed warning (amber
//   strobe) > low fuel in the air (amber breathing) > the aircraft's strobe lights (white
//   flashes) > its beacon (red flashes) > the sky: blue by day, orange at dawn and dusk, deep
//   blue at night, dimmed while parked with the engines off.
#pragma once

#include <cstdint>

#include "effects.h"

namespace luma::app::games {

// One reading of the aircraft (SimConnect's values, in this order: see FlightSimVars()).
struct FlightSimState {
    bool onGround = true, stall = false, overspeed = false, crashed = false;
    bool beacon = false, strobe = false, engineRunning = false;
    double fuelGallons = 0, fuelCapacity = 0;
    int timeOfDay = 1;  // SimConnect's TIME OF DAY: 0 dawn, 1 day, 2 dusk, 3 night
};

// The SimConnect variables asked for, as (name, unit) pairs, in FlightSimState's order.
struct SimVar {
    const char* name;
    const char* unit;
};
inline const SimVar* FlightSimVars(int* count) {
    static const SimVar kVars[] = {
        {"SIM ON GROUND", "Bool"},        {"STALL WARNING", "Bool"},        {"OVERSPEED WARNING", "Bool"},
        {"CRASH FLAG", "Number"},         {"LIGHT BEACON", "Bool"},         {"LIGHT STROBE", "Bool"},
        {"GENERAL ENG COMBUSTION:1", "Bool"}, {"FUEL TOTAL QUANTITY", "Gallons"},
        {"FUEL TOTAL CAPACITY", "Gallons"}, {"TIME OF DAY", "Enum"},
    };
    *count = static_cast<int>(sizeof kVars / sizeof kVars[0]);
    return kVars;
}

// SimConnect's values (doubles, FlightSimVars() order) as a state.
inline FlightSimState FlightSimFromValues(const double* v, int n) {
    FlightSimState s;
    if (n < 10) return s;
    s.onGround = v[0] != 0;
    s.stall = v[1] != 0;
    s.overspeed = v[2] != 0;
    s.crashed = v[3] != 0;
    s.beacon = v[4] != 0;
    s.strobe = v[5] != 0;
    s.engineRunning = v[6] != 0;
    s.fuelGallons = v[7];
    s.fuelCapacity = v[8];
    s.timeOfDay = static_cast<int>(v[9]);
    return s;
}

class FlightSimLighting {
public:
    static constexpr uint64_t kStaleMs = 3000;  // no readings (paused, loading): let go

    void OnState(const FlightSimState& s, uint64_t now) {
        state_ = s;
        lastSeen_ = now;
    }
    // SimConnect's "Sim" event: a flight started (true) or ended (back in the menus).
    void OnSimRunning(bool running) { simRunning_ = running; }
    void OnDisconnected() { lastSeen_ = 0; }

    bool Active(uint64_t now) const { return lastSeen_ && now - lastSeen_ < kStaleMs && simRunning_; }

    fx::Params Current() const {
        using fx::Kind;
        const FlightSimState& s = state_;
        if (s.crashed) return Make(Kind::Breathing, {160, 0, 0}, {}, 0.5);
        if (s.stall) return Make(Kind::Strobe, {255, 0, 0}, {}, 8);
        if (s.overspeed) return Make(Kind::Strobe, {255, 150, 0}, {}, 5);
        if (!s.onGround && s.fuelCapacity > 0 && s.fuelGallons / s.fuelCapacity < 0.1)
            return Make(Kind::Breathing, {255, 150, 0}, {}, 1.0);
        Rgb sky = Sky(s.timeOfDay);
        if (s.onGround && !s.engineRunning) sky = Scale(sky, 0.35);
        if (s.strobe) return Make(Kind::Beat, {255, 255, 255}, sky, 1.0);
        if (s.beacon) return Make(Kind::Beat, {255, 0, 0}, sky, 1.0);
        return Make(Kind::Static, sky);
    }

    const FlightSimState& state() const { return state_; }

private:
    static Rgb Sky(int timeOfDay) {
        switch (timeOfDay) {
        case 0:
        case 2: return {255, 110, 30};  // dawn, dusk
        case 3: return {20, 30, 110};   // night
        default: return {60, 150, 255};  // day
        }
    }
    static fx::Params Make(fx::Kind k, Rgb c1, Rgb c2 = {}, double speed = 0) {
        fx::Params p;
        p.kind = k;
        p.color1 = c1;
        p.color2 = c2;
        p.speed = speed;
        return p;
    }

    FlightSimState state_;
    uint64_t lastSeen_ = 0;
    bool simRunning_ = true;  // connected mid-flight: no "Sim" event yet
};

}  // namespace luma::app::games
