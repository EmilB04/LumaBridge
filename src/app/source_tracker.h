// Which game is lighting the rig right now. Every front-end (game DLLs via IPC, the built-in
// GameSense server) reports as a source; Auto mode shows the source that changed color most
// recently, among those still alive. Pure C++, unit tested.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "color.h"
#include "effects.h"

namespace luma::app {

struct Source {
    uint32_t pid = 0;       // 0 for the built-in GameSense server
    std::string sdk;        // "Logitech LIGHTSYNC", "Razer Chroma", ...
    std::string game;       // process / GameSense game name, for display
    Rgb color{};
    double flashHz = 0;
    uint64_t lastSeen = 0;    // any frame, including keep-alives
    uint64_t lastChange = 0;  // last time the color actually changed
    // Built-in game feeds (CS2, Rocket League, ...) send a whole effect instead of a color.
    bool hasEffect = false;
    fx::Params effect;
};

class SourceTracker {
public:
    // Sources that stop sending (crashed game, DLL without keep-alive) expire after this.
    static constexpr uint64_t kTimeoutMs = 3000;

    void OnFrame(uint32_t pid, const std::string& sdk, const std::string& game, Rgb color,
                 double flashHz, uint64_t now) {
        Source* s = Find(pid, sdk);
        if (!s) {
            sources_.push_back(Make(pid, sdk, game, color, flashHz, now));
            return;
        }
        if (s->color != color || s->flashHz != flashHz) s->lastChange = now;
        s->color = color;
        s->flashHz = flashHz;
        s->game = game;
        s->lastSeen = now;
    }

    // Like OnFrame, for sources that send an effect. `color` (the swatch) is effect.color1.
    void OnEffect(uint32_t pid, const std::string& sdk, const std::string& game, const fx::Params& effect,
                  uint64_t now) {
        Source* s = Find(pid, sdk);
        if (!s) {
            Source n = Make(pid, sdk, game, effect.color1, 0, now);
            n.hasEffect = true;
            n.effect = effect;
            sources_.push_back(n);
            return;
        }
        const fx::Params& e = s->effect;
        if (!s->hasEffect || e.kind != effect.kind || e.color1 != effect.color1 || e.color2 != effect.color2 ||
            e.speed != effect.speed)
            s->lastChange = now;
        s->hasEffect = true;
        s->effect = effect;
        s->color = effect.color1;
        s->game = game;
        s->lastSeen = now;
    }

    void OnRelease(uint32_t pid, const std::string& sdk) {
        sources_.erase(std::remove_if(sources_.begin(), sources_.end(),
                                      [&](const Source& s) { return s.pid == pid && s.sdk == sdk; }),
                       sources_.end());
    }

    // Drops sources that went silent. `keepAlive(source)` lets the owner keep sources that
    // don't send keep-alives (the in-process GameSense server) -- return true to keep.
    template <typename KeepAlive>
    void Prune(uint64_t now, KeepAlive keepAlive) {
        sources_.erase(std::remove_if(sources_.begin(), sources_.end(),
                                      [&](const Source& s) {
                                          return now - s.lastSeen > kTimeoutMs && !keepAlive(s);
                                      }),
                       sources_.end());
    }

    std::optional<Source> Active() const {
        const Source* best = nullptr;
        for (const Source& s : sources_)
            if (!best || s.lastChange > best->lastChange) best = &s;
        if (!best) return std::nullopt;
        return *best;
    }

    const std::vector<Source>& All() const { return sources_; }

private:
    static Source Make(uint32_t pid, const std::string& sdk, const std::string& game, Rgb color, double flashHz,
                       uint64_t now) {
        Source s;
        s.pid = pid;
        s.sdk = sdk;
        s.game = game;
        s.color = color;
        s.flashHz = flashHz;
        s.lastSeen = s.lastChange = now;
        return s;
    }

    Source* Find(uint32_t pid, const std::string& sdk) {
        for (Source& s : sources_)
            if (s.pid == pid && s.sdk == sdk) return &s;
        return nullptr;
    }

    std::vector<Source> sources_;
};

}  // namespace luma::app
