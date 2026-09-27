// SteelSeries GameSense server semantics: the JSON endpoints games POST to, the lighting
// handlers they bind, and the reduction of all of that to one ambient color for Aura.
//
// Games find the server through %PROGRAMDATA%\SteelSeries\SteelSeries Engine 3\coreProps.json
// ({"address": "127.0.0.1:<port>"}), register themselves (game_metadata), bind handlers to
// named events (bind_game_event: "health -> keyboard function-keys, gradient red..green,
// percent mode") and then only send event values (game_event {value: 73}). The server owns
// all the lighting logic, which is what this class re-implements.
//
// Pure C++ with an injected clock; unit tested in tests/test_core.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "color.h"
#include "json.h"

namespace luma::gamesense {

struct Ambient {
    Rgb color{};
    double flashHz = 0;  // 0 = steady
    std::string game;    // GameSense game id of the handler that produced it

    bool operator==(const Ambient& o) const {
        return color == o.color && flashHz == o.flashHz && game == o.game;
    }
    bool operator!=(const Ambient& o) const { return !(*this == o); }
};

class Engine {
public:
    struct Response {
        int status = 200;
        std::string body = "{}";
    };

    explicit Engine(BitmapReduce reduce = BitmapReduce::Average) : reduce_(reduce) {}

    Response Handle(const std::string& path, const std::string& body, uint64_t nowMs) {
        Json j;
        std::string err;
        if (!Json::Parse(body.empty() ? std::string_view("{}") : std::string_view(body), &j, &err))
            return Error("invalid JSON: " + err);
        if (!j.IsObject()) return Error("body must be a JSON object");

        const std::string game = j["game"].String();
        if (path == "/game_metadata") {
            if (game.empty()) return Error("missing game");
            Game& g = games_[game];
            g.lastSeen = nowMs;
            double t = j["deinitialize_timer_length_ms"].Number(0);
            if (t >= 1000 && t <= 60000) g.deinitMs = static_cast<uint64_t>(t);
            return {};
        }
        if (path == "/register_game_event" || path == "/bind_game_event") {
            if (game.empty() || j["event"].String().empty()) return Error("missing game or event");
            Game& g = games_[game];
            g.lastSeen = nowMs;
            Event& e = g.events[j["event"].String()];
            e.minValue = j["min_value"].Number(0);
            e.maxValue = j["max_value"].Number(100);
            if (path == "/bind_game_event") {
                e.handlers.clear();
                const Json& hs = j["handlers"];
                for (size_t i = 0; i < hs.size(); ++i) {
                    Handler h;
                    if (ParseHandler(hs[i], &h)) e.handlers.push_back(std::move(h));
                }
                Recompute(nowMs);
            }
            return {};
        }
        if (path == "/game_event") {
            if (game.empty()) return Error("missing game");
            FireEvent(game, j["event"].String(), j["data"], nowMs);
            Recompute(nowMs);
            return {};
        }
        if (path == "/multiple_game_events") {
            if (game.empty()) return Error("missing game");
            const Json& evs = j["events"];
            for (size_t i = 0; i < evs.size(); ++i)
                FireEvent(game, evs[i]["event"].String(), evs[i]["data"], nowMs);
            Recompute(nowMs);
            return {};
        }
        if (path == "/game_heartbeat") {
            auto it = games_.find(game);
            if (it != games_.end()) it->second.lastSeen = nowMs;
            return {};
        }
        if (path == "/remove_game_event") {
            auto it = games_.find(game);
            if (it != games_.end()) it->second.events.erase(j["event"].String());
            Recompute(nowMs);
            return {};
        }
        if (path == "/remove_game" || path == "/stop_game") {
            auto it = games_.find(game);
            if (it != games_.end()) {
                if (path == "/remove_game") games_.erase(it);
                else Deactivate(it->second);
            }
            Recompute(nowMs);
            return {};
        }
        // Registration-only endpoints with nothing to emulate (screens, tactile, GoLisp
        // handlers, developer-mode toggles): accept so games carry on.
        return {};
    }

    // Drops games whose deinitialize timer ran out. Call periodically.
    void Tick(uint64_t nowMs) {
        bool changed = false;
        for (auto& [name, g] : games_) {
            if (g.active && nowMs - g.lastSeen > g.deinitMs) {
                Deactivate(g);
                changed = true;
            }
        }
        if (changed) Recompute(nowMs);
    }

    bool AnyActive() const {
        for (const auto& kv : games_)
            if (kv.second.active) return true;
        return false;
    }

    // Current ambient output, nullopt when no active game has lit anything.
    const std::optional<Ambient>& Current() const { return ambient_; }
    uint64_t Version() const { return version_; }

private:
    struct ColorSpec {
        enum class Kind { None, Static, Gradient } kind = Kind::None;
        Rgb a{}, b{};  // static: a; gradient: zero=a, hundred=b

        Rgb At(double percent) const {
            if (kind != Kind::Gradient) return a;
            double t = std::clamp(percent, 0.0, 100.0) / 100.0;
            return Rgb{ClampByte(a.r + (b.r - a.r) * t), ClampByte(a.g + (b.g - a.g) * t),
                       ClampByte(a.b + (b.b - a.b) * t)};
        }
    };

    struct Range {
        double low, high;
        ColorSpec color;
        double frequency = 0;
    };

    struct Handler {
        std::string deviceType, zone, mode, contextKey;
        ColorSpec color;
        std::vector<Range> colorRanges;  // "color": [ {low, high, color}, ... ]
        double frequency = 0;
        std::vector<Range> frequencyRanges;
        int weight = 0;

        // Last output
        bool lit = false;
        Rgb out{};
        double outHz = 0;
        uint64_t updatedAt = 0;
    };

    struct Event {
        double minValue = 0, maxValue = 100;
        std::vector<Handler> handlers;
    };

    struct Game {
        std::map<std::string, Event> events;
        uint64_t lastSeen = 0;
        uint64_t deinitMs = 15000;
        bool active = false;
    };

    static Response Error(const std::string& msg) {
        return Response{400, "{\"error\":\"" + JsonEscape(msg) + "\"}"};
    }

    static Rgb ParseRgb(const Json& c) {
        return Rgb{ClampByte(c["red"].Number()), ClampByte(c["green"].Number()),
                   ClampByte(c["blue"].Number())};
    }

    static ColorSpec ParseColorSpec(const Json& c) {
        ColorSpec s;
        if (c.Has("gradient")) {
            s.kind = ColorSpec::Kind::Gradient;
            s.a = ParseRgb(c["gradient"]["zero"]);
            s.b = ParseRgb(c["gradient"]["hundred"]);
        } else if (c.IsObject()) {
            s.kind = ColorSpec::Kind::Static;
            s.a = ParseRgb(c);
        }
        return s;
    }

    // How well a handler's target stands in for "the whole rig". Higher wins.
    static int Weight(const std::string& dev, const std::string& zone, const std::string& mode,
                      bool customZone) {
        if (dev == "tactile" || dev == "screened" || dev.rfind("screened-", 0) == 0) return 0;
        if (mode == "bitmap" || mode == "partial-bitmap") return 100;
        if (dev == "rgb-1-zone") return 90;
        const bool keyboard = dev == "keyboard" || dev == "rgb-per-key-zones";
        if (keyboard && (zone == "all" || zone == "main-keyboard")) return 80;
        if (dev.rfind("rgb-", 0) == 0 && dev.find("-zone") != std::string::npos) return 60;
        if (keyboard) return customZone ? 40 : 50;
        return 30;  // mouse, headset, indicator, ...
    }

    static bool ParseHandler(const Json& h, Handler* out) {
        if (!h.IsObject()) return false;
        out->deviceType = h["device-type"].String();
        out->zone = h["zone"].String();
        out->mode = h["mode"].IsString() ? h["mode"].String() : "color";
        out->contextKey = h["context-frame-key"].String();

        const Json& color = h["color"];
        if (color.IsArray()) {
            for (size_t i = 0; i < color.size(); ++i)
                out->colorRanges.push_back(Range{color[i]["low"].Number(), color[i]["high"].Number(),
                                                 ParseColorSpec(color[i]["color"]), 0});
        } else {
            out->color = ParseColorSpec(color);
        }

        const Json& freq = h["rate"]["frequency"];
        if (freq.IsArray()) {
            for (size_t i = 0; i < freq.size(); ++i)
                out->frequencyRanges.push_back(Range{freq[i]["low"].Number(), freq[i]["high"].Number(),
                                                     ColorSpec{}, freq[i]["frequency"].Number()});
        } else {
            out->frequency = freq.Number(0);
        }

        out->weight = Weight(out->deviceType, out->zone, out->mode, h.Has("custom-zone-keys"));
        return out->weight > 0;
    }

    void FireEvent(const std::string& game, const std::string& event, const Json& data,
                   uint64_t now) {
        auto g = games_.find(game);
        if (g == games_.end()) return;  // unregistered game: GameSense ignores it too
        g->second.lastSeen = now;
        g->second.active = true;
        auto e = g->second.events.find(event);
        if (e == g->second.events.end()) return;

        const double value = data["value"].Number(0);
        const double span = e->second.maxValue - e->second.minValue;
        const double percent =
            span != 0 ? std::clamp((value - e->second.minValue) / span * 100.0, 0.0, 100.0) : 0.0;

        for (Handler& h : e->second.handlers) Evaluate(h, value, percent, data["frame"], now);
    }

    void Evaluate(Handler& h, double value, double percent, const Json& frame, uint64_t now) {
        std::optional<Rgb> c;
        if (h.mode == "bitmap" || h.mode == "partial-bitmap") {
            const Json& bmp = frame["bitmap"];
            c = ReduceColors(
                bmp.size(),
                [&bmp](size_t i) { return Rgb{ClampByte(bmp[i][0].Number()), ClampByte(bmp[i][1].Number()),
                                              ClampByte(bmp[i][2].Number())}; },
                reduce_);
        } else if (h.mode == "context-color") {
            const Json& cc = frame[h.contextKey];
            if (cc.IsObject()) c = ParseRgb(cc);
        } else {
            if (!h.colorRanges.empty()) {
                for (const Range& r : h.colorRanges)
                    if (value >= r.low && value <= r.high) {
                        c = r.color.At(percent);
                        break;
                    }
                if (!c) c = Rgb{};  // no matching range: zone unlit
            } else if (h.color.kind != ColorSpec::Kind::None) {
                c = h.color.At(percent);
            }
            // percent / count modes draw a bar of `value` keys; an empty bar is dark.
            if (c && (h.mode == "percent" || h.mode == "count") && value <= 0) c = Rgb{};
        }
        if (!c) return;

        double hz = h.frequency;
        for (const Range& r : h.frequencyRanges)
            if (value >= r.low && value <= r.high) {
                hz = r.frequency;
                break;
            }

        h.lit = true;
        h.out = *c;
        h.outHz = hz > 0 ? hz : 0;
        h.updatedAt = now;
    }

    static void Deactivate(Game& g) {
        g.active = false;
        for (auto& kv : g.events)
            for (Handler& h : kv.second.handlers) h.lit = false;
    }

    void Recompute(uint64_t /*now*/) {
        const Handler* best = nullptr;
        const std::string* bestGame = nullptr;
        for (const auto& gk : games_) {
            if (!gk.second.active) continue;
            for (const auto& ek : gk.second.events)
                for (const Handler& h : ek.second.handlers) {
                    if (!h.lit) continue;
                    if (!best || h.weight > best->weight ||
                        (h.weight == best->weight && h.updatedAt > best->updatedAt)) {
                        best = &h;
                        bestGame = &gk.first;
                    }
                }
        }
        std::optional<Ambient> next;
        if (best) next = Ambient{best->out, best->outHz, *bestGame};
        if (next != ambient_) {
            ambient_ = next;
            ++version_;
        }
    }

    BitmapReduce reduce_;
    std::map<std::string, Game> games_;
    std::optional<Ambient> ambient_;
    uint64_t version_ = 0;
};

}  // namespace luma::gamesense
