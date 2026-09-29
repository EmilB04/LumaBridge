// Unit tests for the platform-independent core. Builds on any OS:
//   g++ -std=c++17 -I src/core -I src/integrations/razer -I src/integrations/corsair -I src/integrations/alienware -I src/integrations/steelseries tests/test_core.cpp -o test_core && ./test_core
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "color.h"
#include "effects.h"
#include "lighting_state.h"
#include "chroma_translate.h"
#include "aura_usb_protocol.h"
#include "core_props.h"
#include "fake_devices.h"
#include "gamesense_engine.h"
#include "http_parser.h"
#include "ipc.h"
#include "json.h"
#include "source_tracker.h"
#include "game_catalog.h"
#include "game_profiles.h"
#include "cs2_lighting.h"
#include "rocket_league_lighting.h"
#include "dota2_lighting.h"
#include "league_lighting.h"
#include "forza_lighting.h"
#include "flight_sim_lighting.h"
#include "dcs_lighting.h"
#include "scene3d.h"
#include "pc_layout.h"
#include "display_layout.h"
#include "nzxt_kraken.h"
#include "war_thunder_lighting.h"
#include "screen_colors.h"
#include "smbios.h"
#include "lhm.h"
#include "friendly_names.h"
#include "azoth_protocol.h"
#include "azoth_layout.h"
#include "logitech_hidpp.h"
#include "device_lighting.h"
#include "setup_hardware.h"
#include "setup_plan.h"
#include "device_catalog.h"
#include "openrgb_protocol.h"
#include "lamparray.h"
#include "device_sleep.h"
#include "hyperx_ram.h"
#include "hw_sensors.h"
#include "lightfx_state.h"

using namespace luma;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
                         __LINE__, #cond);                              \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static void TestPercent() {
    CHECK(PercentToByte(0) == 0);
    CHECK(PercentToByte(100) == 255);
    CHECK(PercentToByte(50) == 128);
    CHECK(PercentToByte(-5) == 0);
    CHECK(PercentToByte(150) == 255);
    CHECK((FromPercent(100, 0, 50) == Rgb{255, 0, 128}));
}

static void TestAuraPacking() {
    CHECK(ToAuraColor(Rgb{0x11, 0x22, 0x33}) == 0x00332211u);
    CHECK((FromAuraColor(0x00332211u) == Rgb{0x11, 0x22, 0x33}));
}

static void TestCorrection() {
    ColorCorrection cc;
    CHECK((ApplyCorrection(cc, Rgb{10, 128, 255}) == Rgb{10, 128, 255}));
    cc.brightness = 0.5;
    CHECK((ApplyCorrection(cc, Rgb{200, 100, 0}) == Rgb{100, 50, 0}));
    cc.brightness = 1.0;
    cc.gainB = 0.0;
    CHECK((ApplyCorrection(cc, Rgb{255, 255, 255}) == Rgb{255, 255, 0}));
    cc.gainB = 2.0;
    CHECK((ApplyCorrection(cc, Rgb{0, 0, 200}) == Rgb{0, 0, 255}));
    ColorCorrection g;
    g.gamma = 2.0;
    CHECK((ApplyCorrection(g, Rgb{255, 0, 128}).r == 255));
    CHECK((ApplyCorrection(g, Rgb{255, 0, 128}).b == 64));
}

static void SetKey(uint8_t* bmp, int x, int y, Rgb c) {
    uint8_t* p = bmp + (y * kLogiBitmapWidth + x) * kLogiBitmapBytesPerKey;
    p[0] = c.b;
    p[1] = c.g;
    p[2] = c.r;
    p[3] = 255;
}

static void TestHue() {
    CHECK((FromHue(0) == Rgb{255, 0, 0}));
    CHECK((FromHue(120) == Rgb{0, 255, 0}));
    CHECK((FromHue(240) == Rgb{0, 0, 255}));
    CHECK((FromHue(60) == Rgb{255, 255, 0}));
    CHECK((FromHue(360) == Rgb{255, 0, 0}));
    CHECK((FromHue(-120) == Rgb{0, 0, 255}));

    LightingState s;
    s.SetStatic(Rgb{1, 1, 1});
    s.StartSpectrum(3000, 1000);
    CHECK((s.Evaluate(1000) == Rgb{255, 0, 0}));
    CHECK((s.Evaluate(2000) == Rgb{0, 255, 0}));
    CHECK((s.Evaluate(3000) == Rgb{0, 0, 255}));
    CHECK((s.Evaluate(4000) == Rgb{255, 0, 0}));
    CHECK(s.IsAnimating(1000000));
    s.StopEffects();
    CHECK((s.Evaluate(5000) == Rgb{1, 1, 1}));
}

static void TestEffects() {
    using namespace luma::fx;
    Params p;
    p.color1 = Rgb{255, 0, 0};
    p.color2 = Rgb{0, 0, 255};
    p.speed = 1.0;

    p.kind = Kind::Static;
    CHECK((Render(p, 12.3, 5, 16) == Rgb{255, 0, 0}));
    CHECK(!IsAnimated(p));

    p.kind = Kind::Breathing;
    CHECK((Render(p, 0.0, 0, 1) == Rgb{255, 0, 0}));
    CHECK(Render(p, 0.5, 0, 1).IsBlack());

    p.kind = Kind::Strobe;
    CHECK((Render(p, 0.25, 0, 1) == Rgb{255, 0, 0}));
    CHECK(Render(p, 0.75, 0, 1).IsBlack());

    p.kind = Kind::ColorCycle;
    CHECK((Render(p, 1.0 / 3.0, 0, 16) == Render(p, 1.0 / 3.0, 9, 16)));  // same on every LED
    CHECK((Render(p, 1.0 / 3.0, 0, 16) == Rgb{0, 255, 0}));

    p.kind = Kind::RainbowWave;
    CHECK((Render(p, 0.0, 0, 3) == Rgb{255, 0, 0}));
    CHECK((Render(p, 0.0, 1, 3) == Rgb{0, 255, 0}));
    CHECK((Render(p, 0.0, 2, 3) == Rgb{0, 0, 255}));
    CHECK((Render(p, 1.0 / 3.0, 1, 3) == Rgb{255, 0, 0}));  // one step on: red moved right
    CHECK((Render(p, 1.0 / 3.0, 0, 3) == Rgb{0, 0, 255}));
    {
        // Every moving effect travels the same way: towards higher LED numbers (right), and
        // left when reversed. Follow color1's peak for a tenth of a cycle.
        auto peak = [](Params q, double t) {
            int best = 0, bestR = -1;
            for (int i = 0; i < 20; ++i)
                if (Render(q, t, i, 20).r > bestR) bestR = Render(q, t, best = i, 20).r;
            return best;
        };
        for (Kind k : {Kind::Gradient, Kind::Comet}) {
            Params q;
            q.kind = k;
            q.color1 = {255, 0, 0};
            q.color2 = {0, 0, 0};
            q.speed = 1;
            CHECK(peak(q, 0.1) > peak(q, 0.0));
            q.reverse = true;
            CHECK(peak(q, 0.1) != peak(q, 0.0));
            CHECK((peak(q, 0.1) - peak(q, 0.0) + 20) % 20 > 10);  // moved left (wrapping)
        }
    }

    // Customized rainbows.
    Params rb;
    rb.kind = Kind::RainbowWave;
    rb.speed = 0;
    rb.reverse = true;
    CHECK((Render(rb, 0.0, 0, 3) == Rgb{255, 0, 0}));
    CHECK((Render(rb, 0.0, 1, 3) == Rgb{0, 0, 255}));  // the other way round
    rb.reverse = false;
    rb.spread = 2;
    CHECK((Render(rb, 0.0, 0, 4) == Render(rb, 0.0, 2, 4)));  // two rainbows per ring
    rb.spread = 1;
    rb.saturation = 0;
    CHECK((Render(rb, 0.0, 1, 3) == Rgb{255, 255, 255}));  // fully washed out
    rb.saturation = 1;
    rb.hueStart = 120;
    rb.hueSpan = 120;  // green .. blue and back: seamless
    CHECK((Render(rb, 0.0, 0, 4) == Rgb{0, 255, 0}));
    CHECK((Render(rb, 0.0, 2, 4) == Rgb{0, 0, 255}));
    CHECK((Render(rb, 0.0, 1, 4) == Render(rb, 0.0, 3, 4)));
    rb.kind = Kind::ColorCycle;
    rb.speed = 1;
    CHECK((Render(rb, 0.5, 0, 1) == Rgb{0, 0, 255}));

    p.kind = Kind::Gradient;
    p.speed = 0;
    CHECK(!IsAnimated(p));
    CHECK((Render(p, 5.0, 0, 4) == Rgb{255, 0, 0}));   // color1 at the start
    CHECK((Render(p, 5.0, 2, 4) == Rgb{0, 0, 255}));   // color2 half way round
    CHECK((Render(p, 5.0, 1, 4) == Rgb{128, 0, 128}));  // blend in between
    CHECK((Render(p, 5.0, 3, 4) == Rgb{128, 0, 128}));  // seamless back to color1
    p.speed = 1.0;
    CHECK(IsAnimated(p));

    p.kind = Kind::Comet;
    p.color2 = Rgb{};
    CHECK((Render(p, 0.25, 4, 16) == Rgb{255, 0, 0}));  // head at LED 4 at t = 0.25
    CHECK(Render(p, 0.25, 8, 16).IsBlack());            // ahead of the head: background
    const Rgb tail = Render(p, 0.25, 2, 16);
    CHECK(tail.r > 0 && tail.r < 255);                   // tail fades

    p.kind = Kind::Twinkle;
    p.color1 = Rgb{10, 10, 10};
    p.color2 = Rgb{255, 255, 255};
    int sparkling = 0;
    for (int i = 0; i < 64; ++i) sparkling += Render(p, 0.37, i, 64).r > 10;
    CHECK(sparkling > 0 && sparkling < 32);  // a few LEDs sparkle, most don't

    FanLayout fans{3, 4, true};
    std::vector<Rgb> frame;
    p.kind = Kind::RainbowWave;
    p.speed = 0;
    RenderFans(p, 0, fans, &frame);
    CHECK(frame.size() == 12);
    CHECK(frame[0] == frame[4] && frame[1] == frame[5]);  // every fan the same
    fans.repeatPerFan = false;
    RenderFans(p, 0, fans, &frame);
    CHECK(frame[0] != frame[4]);                          // one pattern across all fans
    RenderStrip(p, 0, 5, &frame);
    CHECK(frame.size() == 5);

    fans.repeatPerFan = true;
    RenderFans(p, 0, fans, &frame, 20);  // more LEDs than the layout: the pattern continues
    CHECK(frame.size() == 20);
    CHECK(frame[12] == frame[0] && frame[19] == frame[3]);

    p.kind = Kind::Twinkle;
    p.speed = 1;
    RenderFans(p, 0.37, FanLayout{4, 16, true}, &frame);
    bool fansDiffer = false;
    for (int i = 0; i < 16; ++i) fansDiffer |= frame[i] != frame[16 + i];
    CHECK(fansDiffer);  // twinkle never repeats per fan

    RenderFanTest(fans, &frame, 16);
    CHECK((frame[0] == Rgb{255, 255, 255} && frame[4] == Rgb{255, 255, 255}));  // first LED of each fan
    CHECK((frame[1] == Rgb{255, 0, 0} && frame[5] == Rgb{0, 255, 0}));          // one color per fan
    CHECK(frame[12].IsBlack() && frame[15].IsBlack());                          // past the layout: off
    CHECK((FanLayout{0, 500, true}.TotalLeds() == 120));                        // clamped
}

static void TestBitmap() {
    uint8_t bmp[kLogiBitmapSize];
    std::memset(bmp, 0, sizeof bmp);
    CHECK(ReduceBitmap(bmp, sizeof bmp, BitmapReduce::Average).IsBlack());

    SetKey(bmp, 0, 0, Rgb{200, 0, 0});
    SetKey(bmp, 5, 3, Rgb{0, 0, 100});
    CHECK((ReduceBitmap(bmp, sizeof bmp, BitmapReduce::Average) == Rgb{100, 0, 50}));
    CHECK((ReduceBitmap(bmp, sizeof bmp, BitmapReduce::Brightest) == Rgb{200, 0, 0}));
}

static void TestColorRefs() {
    const uint32_t grid[] = {0x00000000u, 0x000000C8u /* red 200 */, 0x01640000u /* blue 100, flagged */};
    CHECK((ReduceColorRefs(grid, 3, BitmapReduce::Average) == Rgb{100, 0, 50}));
    CHECK((ReduceColorRefs(grid, 3, BitmapReduce::Brightest) == Rgb{200, 0, 0}));
    CHECK(ReduceColorRefs(grid, 1, BitmapReduce::Average).IsBlack());
}

static void TestState() {
    LightingState s;
    const Rgb red{255, 0, 0}, blue{0, 0, 255};

    s.SetStatic(red);
    CHECK(s.Evaluate(0) == red);
    CHECK(!s.IsAnimating(0));

    s.Save();
    s.SetStatic(blue);
    s.Restore();
    CHECK(s.Evaluate(0) == red);

    // Flash blue for 1000ms, 100ms interval, starting at t=1000.
    uint64_t v = s.Version();
    s.StartFlash(blue, 1000, 100, 1000);
    CHECK(s.Version() != v);
    CHECK(s.Evaluate(1000) == blue);
    CHECK(s.Evaluate(1050) == blue);
    CHECK(s.Evaluate(1150).IsBlack());
    CHECK(s.Evaluate(1250) == blue);
    CHECK(s.IsAnimating(1999));
    CHECK(!s.IsAnimating(2000));
    CHECK(s.Evaluate(2000) == red);  // expired -> back to base

    // Infinite pulse.
    s.StartPulse(blue, LightingState::kInfinite, 1000, 0);
    CHECK(s.Evaluate(0) == blue);
    CHECK(s.Evaluate(500).IsBlack());
    CHECK(s.Evaluate(1000) == blue);
    CHECK(s.IsAnimating(1000000));
    s.StopEffects();
    CHECK(s.Evaluate(1000000) == red);

    // A static set cancels a running effect.
    s.StartFlash(blue, LightingState::kInfinite, 100, 0);
    s.SetStatic(Rgb{1, 2, 3});
    CHECK((s.Evaluate(150) == Rgb{1, 2, 3}));
}

static void TestChroma() {
    using namespace luma::chroma;
    uint32_t kb[keyboard::kMaxRow * keyboard::kMaxColumn] = {};
    kb[0] = 0x000000FFu;   // red
    kb[21] = 0x00FF0000u;  // blue
    auto c = TranslateEffect(DeviceClass::Keyboard, keyboard::CUSTOM, kb, BitmapReduce::Average);
    CHECK(c.has_value() && (*c == Rgb{128, 0, 128}));

    uint32_t stat = 0x0000FF00u;  // green
    c = TranslateEffect(DeviceClass::Keyboard, keyboard::STATIC, &stat, BitmapReduce::Average);
    CHECK(c.has_value() && (*c == Rgb{0, 255, 0}));

    c = TranslateEffect(DeviceClass::Keyboard, keyboard::NONE, nullptr, BitmapReduce::Average);
    CHECK(c.has_value() && c->IsBlack());
    CHECK(!TranslateEffect(DeviceClass::Keyboard, keyboard::WAVE, kb, BitmapReduce::Average));
    CHECK(!TranslateEffect(DeviceClass::Keyboard, keyboard::CUSTOM, nullptr, BitmapReduce::Average));

    uint32_t mouseStatic[2] = {7 /* LED id */, 0x00FFFFFFu};
    c = TranslateEffect(DeviceClass::Mouse, mouse::STATIC, mouseStatic, BitmapReduce::Average);
    CHECK(c.has_value() && (*c == Rgb{255, 255, 255}));

    uint32_t gen[3] = {12, 0, 0x00123456u};
    c = TranslateGenericEffect(generic::STATIC, gen);
    CHECK(c.has_value() && (*c == Rgb{0x56, 0x34, 0x12}));

    AmbientSelector sel;  // auto
    CHECK(sel.Update(DeviceClass::ChromaLink, Rgb{1, 1, 1}).has_value());  // only source so far
    CHECK(sel.Update(DeviceClass::Keyboard, Rgb{2, 2, 2}).has_value());    // keyboard takes over
    CHECK(!sel.Update(DeviceClass::ChromaLink, Rgb{3, 3, 3}).has_value()); // ignored now
    CHECK(sel.Driver() == DeviceClass::Keyboard);

    AmbientSelector link(AmbientSource::ChromaLink);
    CHECK(!link.Update(DeviceClass::Keyboard, Rgb{2, 2, 2}).has_value());
    CHECK(link.Update(DeviceClass::ChromaLink, Rgb{3, 3, 3}).has_value());
}

static void TestJson() {
    Json j;
    CHECK(Json::Parse(R"({"a": [1, 2.5, -3e2], "b": {"c": "x\"yé"}, "d": true, "e": null})", &j));
    CHECK(j["a"].size() == 3);
    CHECK(j["a"][1].Number() == 2.5);
    CHECK(j["a"][2].Number() == -300);
    CHECK(j["b"]["c"].String() == "x\"y\xC3\xA9");
    CHECK(j["d"].Bool());
    CHECK(j["e"].IsNull());
    CHECK(j["missing"]["deeper"].IsNull());
    CHECK(!Json::Parse("{\"a\":}", &j));
    CHECK(!Json::Parse("[1,2", &j));
    CHECK(!Json::Parse("{} x", &j));
    std::string deep(200, '[');
    CHECK(!Json::Parse(deep, &j));  // depth limit, no stack overflow
}

static void TestHttp() {
    using namespace luma::http;
    Parser p;
    std::string two = "POST /game_event HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}"
                      "POST /game_heartbeat?x=1 HTTP/1.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                      "3\r\n{\"a\r\n3\r\n\":1\r\n1\r\n}\r\n0\r\n\r\n";
    p.Feed(two.data(), 10);
    Request r;
    CHECK(p.Next(&r) == Parser::Status::NeedMore);
    p.Feed(two.data() + 10, two.size() - 10);
    CHECK(p.Next(&r) == Parser::Status::Done);
    CHECK(r.method == "POST" && r.path == "/game_event" && r.body == "{}" && r.keepAlive);
    CHECK(p.Next(&r) == Parser::Status::Done);
    CHECK(r.path == "/game_heartbeat" && r.body == "{\"a\":1}" && !r.keepAlive);
    CHECK(p.Next(&r) == Parser::Status::NeedMore);
}

static void TestGameSense() {
    using namespace luma::gamesense;
    Engine e;
    auto post = [&](const char* path, const std::string& body, uint64_t t) {
        return e.Handle(path, body, t).status;
    };
    CHECK(post("/game_metadata", R"({"game":"TEST","deinitialize_timer_length_ms":5000})", 0) == 200);
    // health: gradient red(0)..green(100) on the function keys, percent bar
    CHECK(post("/bind_game_event", R"({"game":"TEST","event":"HEALTH","min_value":0,"max_value":200,
        "handlers":[{"device-type":"keyboard","zone":"function-keys","mode":"percent",
        "color":{"gradient":{"zero":{"red":255,"green":0,"blue":0},"hundred":{"red":0,"green":255,"blue":0}}}}]})", 0) == 200);
    CHECK(!e.Current().has_value());
    CHECK(post("/game_event", R"({"game":"TEST","event":"HEALTH","data":{"value":200}})", 10) == 200);
    CHECK(e.Current().has_value() && (e.Current()->color == Rgb{0, 255, 0}));
    post("/game_event", R"({"game":"TEST","event":"HEALTH","data":{"value":100}})", 20);
    CHECK((e.Current()->color == Rgb{128, 128, 0}));
    post("/game_event", R"({"game":"TEST","event":"HEALTH","data":{"value":0}})", 30);
    CHECK(e.Current()->color.IsBlack());  // empty bar

    // A full-keyboard bitmap handler outranks the function-key bar.
    post("/bind_game_event", R"({"game":"TEST","event":"FRAME","handlers":[{"device-type":"rgb-per-key-zones",
        "zone":"all","mode":"bitmap"}]})", 40);
    post("/game_event", R"({"game":"TEST","event":"FRAME","data":{"frame":{"bitmap":[[0,0,200],[0,0,0],[0,0,100]]}}})", 50);
    CHECK((e.Current()->color == Rgb{0, 0, 150}));
    post("/game_event", R"({"game":"TEST","event":"HEALTH","data":{"value":150}})", 60);
    CHECK((e.Current()->color == Rgb{0, 0, 150}));

    // Ranges + flashing.
    post("/bind_game_event", R"({"game":"TEST","event":"ALARM","handlers":[{"device-type":"rgb-1-zone","zone":"one",
        "color":[{"low":0,"high":0,"color":{"red":0,"green":0,"blue":0}},{"low":1,"high":100,"color":{"red":255,"green":0,"blue":0}}],
        "rate":{"frequency":[{"low":50,"high":100,"frequency":4}]}}]})", 70);
    post("/remove_game_event", R"({"game":"TEST","event":"FRAME"})", 75);
    post("/game_event", R"({"game":"TEST","event":"ALARM","data":{"value":80}})", 80);
    CHECK((e.Current()->color == Rgb{255, 0, 0}) && e.Current()->flashHz == 4);
    post("/game_event", R"({"game":"TEST","event":"ALARM","data":{"value":10}})", 90);
    CHECK(e.Current()->flashHz == 0);

    // Deinitialize timer.
    CHECK(e.AnyActive());
    e.Tick(4000);
    CHECK(e.AnyActive());
    e.Tick(6000);
    CHECK(!e.AnyActive() && !e.Current().has_value());

    CHECK(post("/game_event", "not json", 0) == 400);
}

static void TestAuraUsb() {
    using namespace luma::aurausb;
    CHECK(FirmwareRequest()[0] == 0xEC && FirmwareRequest()[1] == 0x82);
    Report m = SetModeRequest(2, kModeDirect);
    CHECK(m[1] == 0x35 && m[2] == 2 && m[5] == 0xFF);

    std::vector<Rgb> leds(45, Rgb{1, 2, 3});
    leds[44] = Rgb{9, 8, 7};
    auto pk = DirectColorRequests(1, leds);
    CHECK(pk.size() == 3);
    CHECK(pk[0][1] == 0x40 && pk[0][2] == 0x01 && pk[0][3] == 0 && pk[0][4] == 20);
    CHECK(pk[1][2] == 0x01 && pk[1][3] == 20 && pk[1][4] == 20);
    CHECK(pk[2][2] == 0x81 && pk[2][3] == 40 && pk[2][4] == 5);  // apply flag on the last
    CHECK(pk[2][5 + 4 * 3] == 9 && pk[2][6 + 4 * 3] == 8 && pk[2][7 + 4 * 3] == 7);
    CHECK(DirectColorRequests(0, {}).empty());

    Report fw{};
    fw[0] = 0xEC; fw[1] = 0x02;
    const char* name = "AUMA0-E6K5-0106";
    for (size_t i = 0; name[i]; ++i) fw[2 + i] = static_cast<uint8_t>(name[i]);
    std::string parsed;
    CHECK(ParseFirmware(fw, &parsed) && parsed == "AUMA0-E6K5-0106");
    Report cfgResp{};
    cfgResp[0] = 0xEC; cfgResp[1] = 0x30; cfgResp[4 + 0x02] = 2; cfgResp[4 + 0x1B] = 5; cfgResp[4 + 0x1D] = 1;
    ConfigTable cfg;
    CHECK(ParseConfig(cfgResp, &cfg) && cfg.ArgbHeaders() == 2 && cfg.MainboardLeds() == 5 && cfg.RgbHeaders() == 1);
    CHECK(!ParseConfig(fw, &cfg));

    // The B550-F's real config table, from the hardware test report.
    const uint8_t b550[] = {0x1E, 0x9F, 0x01, 0x01, 0x00, 0x00, 0x78, 0x3C, 0, 0, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x05, 0x06, 0x02, 0x01, 0xF4};
    ConfigTable real;
    for (size_t i = 0; i < sizeof b550; ++i) real.raw[i] = b550[i];
    CHECK(real.ArgbHeaders() == 1 && real.MainboardLeds() == 5 && real.RgbHeaders() == 2);
    auto ch = BuildChannels(real);
    CHECK(ch.size() == 2);
    CHECK(ch[0].directChannel == 4 && ch[0].leds == 5 && ch[0].auraType == 0x00010000);
    CHECK(ch[1].directChannel == 0 && ch[1].leds == 120 && ch[1].auraType == 0x00011000);
}

static void TestCoreProps() {
    using namespace luma::gamesense;
    CoreProps gg = ParseCoreProps(R"({"address":"127.0.0.1:51248","encrypted_address":"127.0.0.1:51249"})");
    CHECK(gg.valid && !gg.ours && gg.host == "127.0.0.1" && gg.port == 51248);
    CoreProps ours = ParseCoreProps(R"({"address":"127.0.0.1:49713","lumabridge":true})");
    CHECK(ours.valid && ours.ours && ours.port == 49713);
    CHECK(!ParseCoreProps("").valid);
    CHECK(!ParseCoreProps(R"({"address":"127.0.0.1"})").valid);
    CHECK(!ParseCoreProps(R"({"address":"127.0.0.1:99999"})").valid);
    CHECK(!ParseCoreProps(R"({"address":"127.0.0.1:12ab"})").valid);
    CHECK(IsLoopbackHost("127.0.0.1") && IsLoopbackHost("localhost") && !IsLoopbackHost("10.0.0.5"));
}

static void TestCorsairDevices() {
    using namespace luma::corsair;
    auto devs = BuildFakeDevices();
    CHECK(devs.size() == 2);
    CHECK(devs[0].type == CDT_Keyboard && devs[0].leds.size() == 104);
    CHECK(devs[1].type == CDT_Mouse && devs[1].leds.size() == 4);
    CHECK(devs[0].leds.front().id == 1 && devs[1].leds.back().id == 108);
    int w = LedIdForKeyName(devs, 'w');
    CHECK(w != 0 && w == LedIdForKeyName(devs, 'W'));
    CHECK(LedIdForKeyName(devs, '~') == 0);
    LedFrame f;
    f.Set(w, 300, -5, 0);
    Rgb c;
    CHECK(f.Get(w, &c) && (c == Rgb{255, 0, 0}));
    f.Set(1, 0, 0, 255);
    CHECK((f.Reduce(BitmapReduce::Average) == Rgb{128, 0, 128}));
}

static void TestLightFx() {
    using namespace luma::lightfx;
    CHECK((FromPacked(0xFFFF0000u) == Rgb{255, 0, 0}));
    CHECK(FromPacked(0x00FF0000u).IsBlack());  // zero brightness = off
    CHECK((FromStruct(LFX_COLOR{0, 200, 0, 128}) == Rgb{0, 100, 0}));
    State s;
    CHECK(!s.Update());
    s.SetColor(Rgb{1, 2, 3});
    CHECK(s.CurrentColor().IsBlack());  // buffered until Update
    auto cmd = s.Update();
    CHECK(cmd && cmd->kind == Command::Kind::Static && (s.CurrentColor() == Rgb{1, 2, 3}));
    s.SetTiming(300);
    s.Action(LFX_ACTION_PULSE, Rgb{9, 9, 9}, std::nullopt);
    cmd = s.Update();
    CHECK(cmd && cmd->kind == Command::Kind::Pulse && cmd->periodMs == 600);
    s.Action(LFX_ACTION_MORPH, Rgb{1, 1, 1}, Rgb{5, 5, 5});
    CHECK((s.Update()->color == Rgb{5, 5, 5}));
}

static void TestSources() {
    using namespace luma::app;
    SourceTracker t;
    CHECK(!t.Active());
    t.OnFrame(100, "Logitech LIGHTSYNC", "bf1", Rgb{255, 0, 0}, 0, 1000);
    t.OnFrame(0, "SteelSeries GameSense", "CSGO", Rgb{0, 255, 0}, 0, 2000);
    CHECK(t.Active()->game == "CSGO");
    t.OnFrame(100, "Logitech LIGHTSYNC", "bf1", Rgb{255, 0, 0}, 0, 2500);  // keep-alive, no change
    CHECK(t.Active()->game == "CSGO");
    t.OnFrame(100, "Logitech LIGHTSYNC", "bf1", Rgb{0, 0, 255}, 0, 2600);  // real change
    CHECK(t.Active()->game == "bf1");
    t.OnRelease(100, "Logitech LIGHTSYNC");
    CHECK(t.Active()->game == "CSGO");
    t.Prune(2000 + SourceTracker::kTimeoutMs + 1, [](const Source&) { return false; });
    CHECK(!t.Active());

    luma::fx::Params p;
    p.kind = luma::fx::Kind::Gradient;
    p.color1 = Rgb{1, 2, 3};
    t.OnEffect(0, "Counter-Strike 2", "Counter-Strike 2", p, 10000);
    CHECK(t.Active()->hasEffect && t.Active()->effect.kind == luma::fx::Kind::Gradient);
    t.OnEffect(0, "Counter-Strike 2", "Counter-Strike 2", p, 11000);  // same effect: no change
    CHECK(t.Active()->lastChange == 10000 && t.Active()->lastSeen == 11000);
}

static void TestGameCatalog() {
    using namespace luma::app::games;
    PathInfo p = ClassifyPath(L"D:\\SteamLibrary\\steamapps\\common\\Rocket League\\Binaries\\Win64\\RocketLeague.exe");
    CHECK(p.inLibrary && p.store == L"Steam" && p.folder == L"Rocket League");
    p = ClassifyPath(L"C:/Program Files/Epic Games/rocketleague/Binaries/Win64/RocketLeague.exe");
    CHECK(p.inLibrary && p.store == L"Epic Games" && p.folder == L"rocketleague");
    CHECK(ClassifyPath(L"C:\\XboxGames\\Forza Horizon 5\\Content\\ForzaHorizon5.exe").folder == L"Forza Horizon 5");
    CHECK(!ClassifyPath(L"C:\\Program Files (x86)\\Epic Games\\Launcher\\Portal\\EpicGamesLauncher.exe").inLibrary);
    CHECK(!ClassifyPath(L"C:\\Program Files\\Mozilla Firefox\\firefox.exe").inLibrary);
    CHECK(!ClassifyPath(L"D:\\SteamLibrary\\steamapps\\common\\x.exe").inLibrary);

    CHECK(IsHelperExe(L"UnityCrashHandler64.exe") && IsHelperExe(L"EasyAntiCheat_EOS.exe") &&
          IsHelperExe(L"RiotClientServices_Launcher.exe"));
    CHECK(!IsHelperExe(L"RocketLeague.exe") && !IsHelperExe(L"bf1.exe"));
    CHECK(IsAntiCheatName(L"EasyAntiCheat_EOS.exe") && IsAntiCheatName(L"BEService_x64.exe") &&
          IsAntiCheatName(L"vgc.exe") && IsAntiCheatName(L"BattlEye"));
    CHECK(!IsAntiCheatName(L"RocketLeague.exe") && !IsAntiCheatName(L"bf1.exe"));

    CHECK(std::strcmp(LightingSdkForModule(L"RzChromaSDK64.dll"), "Razer Chroma") == 0);
    CHECK(std::strcmp(LightingSdkForModule(L"LogitechLed.dll"), "Logitech LIGHTSYNC") == 0);
    CHECK(std::strcmp(LightingSdkForModule(L"LumaBridge_x64.dll"), "Logitech LIGHTSYNC") == 0);
    CHECK(std::strcmp(LightingSdkForModule(L"CUESDK.x64_2019.dll"), "Corsair iCUE") == 0);
    CHECK(LightingSdkForModule(L"d3d11.dll") == nullptr);

    CHECK(Normalize("Rocket League") == Normalize("ROCKETLEAGUE"));
    CHECK(CleanName(L"Battlefield\u2122 1") == L"Battlefield 1" && CleanName(L"Game\u00AE") == L"Game");
    CHECK(IsGenericProductName(L"Unreal Engine") && IsGenericProductName(L"") && !IsGenericProductName(L"Rocket League"));

    const std::string vdf = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
                            "\t\t\"apps\" { \"730\" \"1\" }\n\t}\n\t\"1\" { \"path\" \"D:\\\\SteamLibrary\" }\n}";
    const auto paths = VdfValues(vdf, "path");
    CHECK(paths.size() == 2 && paths[0] == "C:\\Program Files (x86)\\Steam" && paths[1] == "D:\\SteamLibrary");
    const std::string acf = "\"AppState\" { \"appid\" \"1238840\" \"name\" \"Battlefield\u2122 1\" \"installdir\" \"Battlefield 1\" }";
    CHECK(VdfValues(acf, "installdir").at(0) == "Battlefield 1" && VdfValues(acf, "NAME").size() == 1);

    CHECK(ClassifySupport(true, false, false, false) == Support::Active);
    CHECK(ClassifySupport(false, true, false, true) == Support::Known);
    CHECK(ClassifySupport(false, false, true, true) == Support::SdkLoaded);
    CHECK(ClassifySupport(false, false, false, true) == Support::None);
    CHECK(ClassifySupport(false, false, false, false) == Support::Unknown);
    CHECK(SupportsLighting(Support::SdkLoaded) && !SupportsLighting(Support::Unknown));
}

static luma::Json J(const char* text) {
    luma::Json j;
    CHECK(luma::Json::Parse(text, &j));
    return j;
}

static void TestGameProfiles() {
    using namespace luma::app::games;
    CHECK(FindProfile("CS2.exe", "")->feed == Feed::Cs2Gsi);
    CHECK(FindProfile("RocketLeague.exe", "Rocket League")->kind == ProfileKind::BuiltIn);
    CHECK(FindProfile("aces.exe", "War Thunder")->feed == Feed::WarThunderApi);
    CHECK(FindProfile("x.exe", "BOMBANANA!")->kind == ProfileKind::NoSupport);
    CHECK(FindProfile("x.exe", "RV There Yet?")->kind == ProfileKind::NoSupport);
    CHECK(FindProfile("DSX.exe", "DSX")->kind == ProfileKind::NotAGame);
    CHECK(FindProfile("bf1.exe", "")->kind == ProfileKind::VendorSdk);
    CHECK(FindProfile("bf2042.exe", "")->blocked && FindProfile("bf1.exe", "")->blocked);
    CHECK(FindProfile("", "Euro Truck Simulator 2")->kind == ProfileKind::VendorSdk);
    CHECK(FindProfile("", "The Last of Us\xE2\x84\xA2 Part I")->kind == ProfileKind::VendorSdk);  // with the TM sign
    CHECK(FindProfile("r5apex.exe", "")->kind == ProfileKind::VendorSdk);
    CHECK(FindProfile("", "Tom Clancy's Rainbow Six Siege")->kind == ProfileKind::NoSupport);
    CHECK(FindProfile("wallpaper64.exe", "")->kind == ProfileKind::NotAGame);
    CHECK(FindProfile("bf6.exe", "")->kind == ProfileKind::VendorSdk && FindProfile("bf6.exe", "")->blocked);
    CHECK(!FindProfile("overwatch.exe", "")->blocked && !FindProfile("cs2.exe", "")->blocked);
    // Blocked games light Logitech gear through G HUB: LumaBridge hands the gear to them.
    CHECK(std::strstr(FindProfile("bf2042.exe", "")->how, "Logitech") != nullptr);
    CHECK(FindProfile("notepad.exe", "Some Game") == nullptr);
    CHECK(std::strcmp(ProfileByKey("warthunder")->title, "War Thunder") == 0);
}

static void TestCs2() {
    using namespace luma::app::games;
    using luma::fx::Kind;
    Cs2Lighting cs;
    CHECK(!cs.Active(0));
    CHECK(!cs.OnState(J(R"({"auth":{"token":"x"}})"), 1000, "lumabridge"));  // wrong token
    const char* base = R"({"auth":{"token":"lumabridge"},"provider":{"steamid":"1"},"map":{"phase":"live"},
        "round":{"phase":"live"},"player":{"steamid":"1","team":"T","activity":"playing",
        "state":{"health":100,"flashed":0,"burning":0,"round_kills":0,"round_killhs":0}}})";
    CHECK(cs.OnState(J(base), 1000, "lumabridge"));
    CHECK(cs.Active(1000));
    CHECK((cs.Current(1000).kind == Kind::Static && cs.Current(1000).color1 == Cs2Lighting::kT));
    CHECK(cs.OnState(J(R"({"provider":{"steamid":"1"},"map":{},"round":{"phase":"live"},"player":{"steamid":"1",
        "team":"T","activity":"playing","state":{"health":20,"round_kills":1,"round_killhs":0}}})"), 2000));
    CHECK((cs.Current(2000).color1 == luma::Rgb{40, 255, 80}));   // kill flash
    CHECK(cs.Current(3000).kind == Kind::Breathing);             // then low health
    CHECK(cs.OnState(J(R"({"provider":{"steamid":"1"},"map":{},"round":{"phase":"live","bomb":"planted"},
        "player":{"steamid":"1","team":"T","activity":"playing","state":{"health":100,"round_kills":1}}})"), 4000));
    // The bomb's beat: counted from the plant (a shared start for every device), speeding up.
    const luma::fx::Params beat = cs.Current(4000);
    CHECK(beat.kind == Kind::Beat && beat.epoch == 4000 && beat.speed == 1.0 && beat.speedEnd == 5.0 &&
          beat.rampSeconds == 40.0);
    CHECK(luma::fx::SameParams(cs.Current(4000), cs.Current(30000)));  // one effect all along: nothing restarts
    CHECK(cs.OnState(J(R"({"provider":{"steamid":"1"},"map":{},"round":{"phase":"over","bomb":"exploded","win_team":"T"},
        "player":{"steamid":"1","team":"T","activity":"playing","state":{"health":100}}})"), 50000));
    CHECK(cs.Current(50000).kind == Kind::Strobe);         // explosion
    CHECK(cs.Current(52500).kind == Kind::RainbowWave);    // round won
    CHECK(cs.OnState(J(R"({"provider":{"steamid":"1"},"player":{"activity":"menu"}})"), 60000));
    CHECK(!cs.Active(60000));
    CHECK(!cs.Active(60000 + Cs2Lighting::kStaleMs + 1));
}

static void TestRocketLeague() {
    using namespace luma::app::games;
    using luma::fx::Kind;
    std::string stream = R"({"Event":"UpdateState","Data":"{\"Game\":{\"bOvertime\":false,\"Teams\":[{\"TeamNum\":0,\"ColorPrimary\":\"1873FF\"},{\"TeamNum\":1,\"ColorPrimary\":\"FF6E00\"}]}}"}{"Event":"Goal)";
    std::vector<std::string> msgs;
    TakeJsonObjects(&stream, &msgs);
    CHECK(msgs.size() == 1 && stream == R"({"Event":"Goal)");  // half a message waits for the rest
    stream += R"(Scored","Data":{"Scorer":{"Name":"x","TeamNum":1}}} )";
    TakeJsonObjects(&stream, &msgs);
    CHECK(msgs.size() == 2 && stream.empty());
    std::string braces = R"({"Event":"X","Data":{"Name":"a}{b"}})";
    std::vector<std::string> one;
    TakeJsonObjects(&braces, &one);
    CHECK(one.size() == 1);  // braces inside strings don't split

    const std::string ini = "[TAGame.MatchStatsExporter_TA]\r\nPort=49123\r\nPacketSendRate=0\r\n";
    CHECK(StatsIniValue(ini, "PacketSendRate", -1) == 0 && StatsIniValue(ini, "Port", 0) == 49123);
    const std::string on = WithPacketSendRate(ini, 30);
    CHECK(StatsIniValue(on, "PacketSendRate", -1) == 30 && StatsIniValue(on, "Port", 0) == 49123);
    CHECK(on.size() == ini.size() + 1);  // only the value changed
    const std::string added = WithPacketSendRate("[TAGame.MatchStatsExporter_TA]\nPort=1\n", 30);
    CHECK(StatsIniValue(added, "PacketSendRate", -1) == 30 && StatsIniValue(added, "Port", 0) == 1);
    CHECK(StatsIniValue(WithPacketSendRate("", 30), "PacketSendRate", -1) == 30);

    RocketLeagueLighting rl;
    CHECK(rl.OnMessage(J(msgs[0].c_str()), 1000));
    CHECK(rl.Active(1000));
    CHECK((rl.Current(1000).kind == Kind::Gradient && rl.Current(1000).color2 == luma::Rgb{255, 110, 0}));
    CHECK(rl.OnMessage(J(msgs[1].c_str()), 2000));
    CHECK((rl.Current(2000).kind == Kind::Strobe && rl.Current(2000).color1 == luma::Rgb{255, 110, 0}));
    CHECK(rl.Current(6000).kind == Kind::Comet);
    CHECK(rl.Current(9000).kind == Kind::Gradient);
    CHECK(rl.OnMessage(J(R"({"Event":"MatchEnded","Data":{"WinnerTeamNum":0}})"), 10000));
    CHECK((rl.Current(10000).kind == Kind::Breathing && rl.Current(10000).color1 == luma::Rgb{24, 115, 255}));
    CHECK(rl.OnMessage(J(R"({"Event":"MatchDestroyed","Data":{}})"), 11000));
    CHECK(!rl.Active(11000) && rl.myTeam() == -1);

    // Your team (the camera's target): orange -> orange, bright to dim, not blue.
    RocketLeagueLighting me;
    CHECK(me.OnMessage(J(R"({"Event":"UpdateState","Data":{"Game":{"bHasTarget":true,"Target":{"Name":"me","TeamNum":1},)"
                         R"("Teams":[{"TeamNum":0,"ColorPrimary":"1873FF"},{"TeamNum":1,"ColorPrimary":"FF6E00"}]}}})"),
                       1000));
    CHECK(me.myTeam() == 1);
    CHECK((me.Current(1000).kind == Kind::Gradient && me.Current(1000).color1 == luma::Rgb{255, 110, 0} &&
           me.Current(1000).color2 == luma::Rgb{89, 38, 0}));
    // No target (spectating): the team stays; a new match forgets it.
    CHECK(me.OnMessage(J(R"({"Event":"UpdateState","Data":{"Game":{"bHasTarget":false,"Teams":[]}}})"), 2000));
    CHECK(me.myTeam() == 1);
    CHECK(me.OnMessage(J(R"({"Event":"MatchCreated","Data":{}})"), 3000));
    CHECK((me.myTeam() == -1 && me.Current(3000).color2 == luma::Rgb{255, 110, 0}));
}

static void TestDota2() {
    using namespace luma::app::games;
    using luma::fx::Kind;
    Dota2Lighting d;
    // CS2's posts aren't Dota's; a wrong token neither.
    CHECK(!d.OnState(J(R"({"provider":{"appid":730},"auth":{"token":"lumabridge"}})"), 1000, "lumabridge"));
    CHECK(!d.OnState(J(R"({"provider":{"appid":570},"auth":{"token":"x"}})"), 1000, "lumabridge"));
    auto post = [&](const char* game, uint64_t now) {
        return d.OnState(J((std::string(R"({"provider":{"name":"Dota 2","appid":570},"auth":{"token":"lumabridge"},)") + game +
                            "}").c_str()),
                         now, "lumabridge");
    };
    CHECK(post(R"("map":{"game_state":"DOTA_GAMERULES_STATE_STRATEGY_TIME","daytime":true,"win_team":"none"},
        "player":{"team_name":"dire","kills":0},"hero":{"alive":true,"health_percent":100})", 1000));
    CHECK(d.Active(1000) && d.team() == "dire" && d.Current(1000).kind == Kind::Breathing);
    CHECK(post(R"("map":{"game_state":"DOTA_GAMERULES_STATE_GAME_IN_PROGRESS","daytime":true,"win_team":"none"},
        "player":{"team_name":"dire","kills":0},"hero":{"alive":true,"health_percent":90})", 2000));
    CHECK((d.Current(2000).kind == Kind::Static && d.Current(2000).color1 == Dota2Lighting::kDire));
    CHECK(post(R"("map":{"game_state":"DOTA_GAMERULES_STATE_GAME_IN_PROGRESS","daytime":false,"win_team":"none"},
        "player":{"team_name":"dire","kills":1},"hero":{"alive":true,"health_percent":20})", 3000));
    CHECK(d.Current(3000).kind == Kind::Strobe);     // a kill
    CHECK(d.Current(4000).kind == Kind::Breathing);  // then low health
    CHECK(post(R"("map":{"game_state":"DOTA_GAMERULES_STATE_GAME_IN_PROGRESS","daytime":false,"win_team":"none"},
        "player":{"team_name":"dire","kills":1},"hero":{"alive":false,"health_percent":0})", 5000));
    CHECK((d.Current(5000).color1 == luma::Scale(Dota2Lighting::kDire, 0.12)));  // dead
    CHECK(post(R"("map":{"game_state":"DOTA_GAMERULES_STATE_POST_GAME","daytime":true,"win_team":"dire"},
        "player":{"team_name":"dire","kills":1},"hero":{"alive":true,"health_percent":100})", 6000));
    CHECK(d.Current(6000).kind == Kind::RainbowWave);  // won
    CHECK(d.Active(6000) && !d.Active(6000 + 20000));
}

static void TestLeague() {
    using namespace luma::app::games;
    using luma::fx::Kind;
    LeagueLighting l;
    const char* base = R"({"activePlayer":{"riotId":"Me#EUW","riotIdGameName":"Me","summonerName":"Me",
        "championStats":{"currentHealth":%d,"maxHealth":1000}},
        "allPlayers":[{"riotId":"Me#EUW","riotIdGameName":"Me","team":"CHAOS","isDead":%s},
                      {"riotId":"Foe#EUW","riotIdGameName":"Foe","team":"ORDER","isDead":false}],
        "events":{"Events":[%s]}})";
    auto data = [&](int hp, const char* dead, const char* events) {
        char buf[2048];
        snprintf(buf, sizeof buf, base, hp, dead, events);
        return J(buf);
    };
    // The first answer: kills from before don't replay.
    l.OnGameData(data(900, "false", R"({"EventID":0,"EventName":"GameStart"},{"EventID":1,"EventName":"ChampionKill","KillerName":"Me"})"), 1000);
    l.Prime();
    CHECK(l.Active(1000) && l.team() == "CHAOS");
    CHECK((l.Current(1000).kind == Kind::Static && l.Current(1000).color1 == LeagueLighting::kRedSide));
    l.OnGameData(data(900, "false", R"({"EventID":2,"EventName":"ChampionKill","KillerName":"Me"})"), 2000);
    CHECK(l.Current(2000).kind == Kind::Strobe);  // my kill
    l.OnGameData(data(900, "false", R"({"EventID":3,"EventName":"DragonKill","DragonType":"Water","KillerName":"Me"})"), 3000);
    CHECK((l.Current(3000).kind == Kind::Breathing && l.Current(3000).color1 == luma::Rgb{0, 170, 255}));
    l.OnGameData(data(900, "false", R"({"EventID":4,"EventName":"DragonKill","DragonType":"Fire","KillerName":"Foe"})"), 6000);
    CHECK(l.Current(6000).kind == Kind::Static);  // their dragon: nothing
    l.OnGameData(data(200, "false", ""), 7000);
    CHECK((l.Current(7000).kind == Kind::Breathing && l.Current(7000).color1 == luma::Rgb{255, 0, 0}));  // low health
    l.OnGameData(data(0, "true", ""), 8000);
    CHECK((l.Current(8000).color1 == luma::Scale(LeagueLighting::kRedSide, 0.12)));  // dead
    l.OnGameData(data(900, "false", R"({"EventID":5,"EventName":"GameEnd","Result":"Win"})"), 9000);
    CHECK(l.Current(9000).kind == Kind::RainbowWave);
    CHECK(!l.Active(9000 + LeagueLighting::kStaleMs + 1));
}

static void TestScene3d() {
    using namespace luma::app::s3d;
    Camera cam;
    cam.target = {0, 0, 0};
    cam.yaw = 0;
    cam.pitch = 0.0001f;
    cam.distance = 100;
    Viewport vp{0, 0, 200, 100};
    // Looking along -z from +z: the target lands in the middle, +x to the right, +y up.
    Projected c = Project(cam, vp, {0, 0, 0});
    CHECK(c.visible && std::fabs(c.sx - 100) < 0.01f && std::fabs(c.sy - 50) < 0.1f && std::fabs(c.z - 100) < 0.01f);
    CHECK(Project(cam, vp, {10, 0, 0}).sx > 100 && Project(cam, vp, {0, 10, 0}).sy < 50);
    CHECK(!Project(cam, vp, {0, 0, 150}).visible);  // behind the eye
    // The screen ray back through a projected point finds it again.
    V3 hit;
    cam.pitch = 0.6f;
    const Projected g = Project(cam, vp, {12, 0, -30});
    CHECK(HitPlaneY(ScreenRay(cam, vp, g.sx, g.sy), 0, &hit) && std::fabs(hit.x - 12) < 0.05f && std::fabs(hit.z + 30) < 0.05f);

    // A box shows its three faces towards the eye; a room (inward walls) the three away.
    cam.yaw = 0.7f;
    cam.pitch = 0.5f;
    Scene box;
    box.Box({-5, -5, -5}, {5, 5, 5}, Rgba(200, 200, 200), 1);
    CHECK(Render(box, cam, vp).size() == 3);
    Scene room;
    room.Room({-5, -5, -5}, {5, 5, 5}, Rgba(200, 200, 200), 2);
    CHECK(Render(room, cam, vp).size() == 3);
    // Painter's order: far first; the nearer box wins the pick; walls always draw first.
    Scene two;
    two.Room({-40, -40, -40}, {40, 40, 40}, Rgba(50, 50, 50), 9);
    two.Box({-3, -3, -3}, {3, 3, 3}, Rgba(255, 0, 0), 1);
    two.xf = Transform::Translate(Normalize(cam.Eye() - cam.target) * 20.f);
    two.Box({-1, -1, -1}, {1, 1, 1}, Rgba(0, 255, 0), 2);
    const auto items = Render(two, cam, vp);
    CHECK(items.front().background);
    for (size_t i = 1; i < items.size(); ++i)
        if (!items[i].background && !items[i - 1].background) CHECK(items[i - 1].depth >= items[i].depth);
    CHECK(Pick(items, vp.w / 2, vp.h / 2) == 2);
    CHECK(Pick(items, 1, 1) == 9 || Pick(items, 1, 1) == -1);
    // Emissive faces keep their color; lit ones are shaded.
    Scene led;
    led.xf = Transform{};
    led.Box({-1, -1, -1}, {1, 1, 1}, Rgba(255, 0, 0), 1, kEmissive);
    for (const auto& d : Render(led, cam, vp)) CHECK(d.color == Rgba(255, 0, 0));
    // A mirrored placement still shows a box's outside faces (three of them), and a biased
    // plate sorts behind what sits on it.
    Scene mirror;
    mirror.xf = Transform{}.MirroredX();
    mirror.Box({-5, -5, -5}, {5, 5, 5}, Rgba(200, 200, 200), 1);
    CHECK(Render(mirror, cam, vp).size() == 3);
    Scene plate;
    plate.bias = 40;
    plate.Box({-20, -1, -20}, {20, 0, 20}, Rgba(60, 60, 60), 1);
    plate.bias = 0;
    plate.Box({15, 0, -18}, {17, 1, -16}, Rgba(255, 0, 0), 2);  // a key at the plate's far corner
    const auto pl = Render(plate, cam, vp);
    CHECK(pl.back().id == 2);
    // A transform facing +x puts local z along x.
    const Transform f = Transform::Facing({1, 0, 0}, {5, 0, 0});
    const V3 p = f.Apply({0, 0, 2});
    CHECK(std::fabs(p.x - 7) < 1e-4f && std::fabs(p.y) < 1e-4f);
    const float square[] = {0, 0, 10, 0, 10, 10, 0, 10};
    CHECK(InsidePolygon(square, 4, 5, 5) && !InsidePolygon(square, 4, 15, 5));
}

static void TestPcLayout() {
    using namespace luma::app::pc;
    // 6 fans, 3 of them RGB: 3 in front (the RGB ones), 1 at the back, 2 on top.
    const Layout g = Guess(3, 3);
    CHECK(g.Fans() == 6 && g.RgbFans() == 3 && g.guessed);
    int front = 0, back = 0, top = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (g.slots[static_cast<size_t>(i)] == SlotFan::None) continue;
        const Mount m = Slots()[static_cast<size_t>(i)].mount;
        front += m == Mount::Front;
        back += m == Mount::Back;
        top += m == Mount::Top;
    }
    CHECK(front == 3 && back == 1 && top == 2);
    CHECK(g.ChainIndex(0) == 0 && g.ChainIndex(2) == 2 && g.ChainIndex(3) == -1);
    CHECK(!g.Exhaust(0) && g.Exhaust(3) && g.Exhaust(4));
    CHECK(Guess(0, 20).Fans() == kSlots);  // never more than there are slots

    Layout l = g;
    CycleSlot(&l, 3);  // the back fan becomes an RGB fan
    CHECK(l.slots[3] == SlotFan::Rgb && !l.guessed && l.ChainIndex(3) == 3);
    CycleSlot(&l, 3);
    CHECK(l.slots[3] == SlotFan::None);
    l.exhaust[static_cast<size_t>(Mount::Top)] = false;
    l.turn = 2;
    Layout back2;
    CHECK(Decode(Encode(l), &back2) && back2 == l);
    CHECK(Encode(Guess(3, 3)).rfind("v1;slots=222111000;exhaust=0110;turn=0", 0) == 0);
    // The CPU cooler: air unless said otherwise (layouts saved before AIOs), an AIO's radiator
    // on top or in front.
    CHECK(Decode("v1;slots=222111000;exhaust=0110;turn=0", &back2) && back2.cooler == Cooler::Air);
    l.cooler = Cooler::Aio;
    l.radiator = Mount::Front;
    CHECK(Decode(Encode(l), &back2) && back2 == l && Encode(l).find("cooler=aio-front") != std::string::npos);
    l.radiator = Mount::Top;
    CHECK(Decode(Encode(l), &back2) && back2.cooler == Cooler::Aio && back2.radiator == Mount::Top && !back2.pumpRgb);
    l.pumpRgb = true;
    CHECK(Decode(Encode(l), &back2) && back2 == l && Encode(l).find("cooler=aio-top-rgb") != std::string::npos);
    l.radiator = Mount::Front;
    CHECK(Decode(Encode(l), &back2) && back2.radiator == Mount::Front && back2.pumpRgb);
    // Fans per position: 3 front, 2 top, 1 back, whatever was there.
    Layout counts = Guess(0, 1);
    SetFansAt(&counts, Mount::Front, 3);
    SetFansAt(&counts, Mount::Top, 2);
    SetFansAt(&counts, Mount::Back, 1);
    CHECK(FansAt(counts, Mount::Front) == 3 && FansAt(counts, Mount::Top) == 2 && FansAt(counts, Mount::Back) == 1 &&
          counts.Fans() == 6 && !counts.guessed && SlotsAt(Mount::Top) == 3);
    SetFansAt(&counts, Mount::Front, 1);
    CHECK(FansAt(counts, Mount::Front) == 1 && counts.slots[0] != SlotFan::None && counts.slots[1] == SlotFan::None);
    CHECK(!Decode("", &back2) && !Decode("v1;slots=22;exhaust=0110", &back2) && !Decode("v1;slots=922110000;exhaust=0110", &back2));

    // Airflow starts outside an intake and ends outside an exhaust.
    const auto p0 = AirflowPoint(g, 0, 0, 0, 7), p1 = AirflowPoint(g, 0, 0, 1, 7);
    CHECK(p0.z > kCaseD / 2 - 2);                            // in front of the front fans
    CHECK(p1.z < -kCaseD / 2 + 2 || p1.y > kCaseH - 2);      // behind the back or above the top
    Layout none;
    CHECK(AirflowPoint(none, 0, 0, 1, 3).z < -kCaseD / 2);   // no fans: out the back vents
}

static void TestAioCatalog() {
    using namespace luma::app::catalog;
    const AioModel* k = FindAio({{0x046D, 0xC547}, {0x1E71, 0x3008}});
    CHECK(k && std::string(k->name).find("Kraken Z") != std::string::npos && k->lcd);
    CHECK(FindAio({{0x1E71, 0x2007}}) && !FindAio({{0x1E71, 0x2007}})->lcd);
    CHECK(!FindAio({{0x1E71, 0x1234}, {0x046D, 0xC547}}));  // NZXT, but not a cooler LumaBridge knows
    CHECK(FindAio({{0x1E71, 0x3014}}) && FindAio({{0x1E71, 0x3014}})->lcd);  // Kraken 2024 Plus
    CHECK(FindAio({{0x1E71, 0x3020}}) && FindAio({{0x1E71, 0x3020}})->pid == 0x3020);  // a newer screen Kraken
    CHECK(!FindAio({{0x1E71, 0x2019}}));  // NZXT's RGB & fan controller is no cooler
    CHECK(FindAio({{0x1E71, 0x2019}}, {{0x1E71, 0x2040}}) && FindAio({}, {{0x1E71, 0x2040}})->pid == 0x2040);  // named "Kraken"
    // A Kraken status report: 31.5 °C liquid, pump at 2400 RPM / 60 %, fans (screen models) 1200 RPM / 45 %.
    uint8_t r[64] = {0x75, 0x01};
    r[15] = 31;
    r[16] = 5;
    r[17] = 2400 & 0xFF;
    r[18] = 2400 >> 8;
    r[19] = 60;
    r[23] = 1200 & 0xFF;
    r[24] = 1200 >> 8;
    r[25] = 45;
    auto st = luma::app::nzxt::Parse(r, sizeof r, true);
    CHECK(st.valid && std::fabs(st.liquidC - 31.5) < 1e-9 && st.pumpRpm == 2400 && st.pumpDuty == 60 && st.fanRpm == 1200);
    CHECK(luma::app::nzxt::Parse(r, sizeof r, false).fanRpm == -1);  // the X models don't report fans
    r[15] = r[16] = 0xFF;
    CHECK(!luma::app::nzxt::Parse(r, sizeof r, true).valid);  // no reading yet
    r[0] = 0x11;
    CHECK(!luma::app::nzxt::Parse(r, sizeof r, true).valid);  // not a status report
}

static void TestDisplayLayout() {
    using namespace luma::app::displays;
    // Two 27" 1440p monitors side by side, the primary on the left.
    Display a, b;
    a.primary = true;
    a.w = b.w = 2560;
    a.h = b.h = 1440;
    a.widthCm = b.widthCm = 59.7f;
    a.heightCm = b.heightCm = 33.6f;
    b.x = 2560;
    auto p = Arrange({a, b});
    CHECK(p.size() == 2 && std::fabs(p[0].x) < 0.01f && p[0].yaw == 0);
    CHECK(std::fabs(p[1].x - (59.7f + 1.5f)) < 0.01f && p[1].yaw < 0 && p[1].z > 0);  // turned towards you
    CHECK(std::fabs(p[0].bottom - 12) < 0.01f && std::fabs(p[1].bottom - 12) < 0.01f);
    // A 1080p 24" on the left of a 4K 27" (other DPI): still edge to edge.
    Display k, s;
    k.primary = true;
    k.w = 3840;
    k.h = 2160;
    k.widthCm = 59.7f;
    k.heightCm = 33.6f;
    s.x = -1920;
    s.w = 1920;
    s.h = 1080;
    s.widthCm = 53.1f;
    s.heightCm = 29.9f;
    p = Arrange({k, s});
    CHECK(std::fabs(p[1].x + (59.7f + 53.1f) / 2 + 1.5f) < 0.01f && p[1].yaw > 0);
    // One above the primary: higher up, straight.
    Display top = a;
    top.primary = false;
    top.x = 0;
    top.y = -1440;
    p = Arrange({a, top});
    CHECK(std::fabs(p[1].x) < 0.01f && std::fabs(p[1].bottom - (12 + 33.6f + 1.5f)) < 0.01f && p[1].yaw == 0);
    // No size reported: a guess by resolution; portrait turned the right way.
    Display q;
    q.w = 1080;
    q.h = 1920;
    float w, h;
    ScreenSize(q, &w, &h);
    CHECK(h > w && std::fabs(std::sqrt(w * w + h * h) - 24 * 2.54f) < 0.5f);
    CHECK(Arrange({}).empty());
    // A small screen (a 7" sensor panel) rests on the desk, no stand.
    Display tiny;
    tiny.x = 2560;
    tiny.w = 1024;
    tiny.h = 600;
    tiny.widthCm = 15.4f;
    tiny.heightCm = 9.0f;
    p = Arrange({a, tiny});
    CHECK(p[1].resting && p[1].bottom < 1 && !p[0].resting && p[1].z > 10);
    // A 10.1" screen right under the primary (and a portrait one to its right): it stands on
    // the desk beneath it, and the primary sits right on top of it.
    Display small, side;
    small.x = 400;
    small.y = 1440;
    small.w = 1280;
    small.h = 800;
    small.widthCm = 21.7f;
    small.heightCm = 13.6f;
    side.x = 2560;
    side.y = -200;
    side.w = 1080;
    side.h = 1920;
    p = Arrange({a, side, small});
    CHECK(p[2].resting && p[2].bottom < 1 && p[2].lean < 0.2f && p[2].z > p[0].z);
    CHECK(std::fabs(p[0].bottom - (0.3f + 13.6f + 0.3f)) < 0.01f);  // no gap between them
    CHECK(p[2].x > -59.7f / 2 && p[2].x + 21.7f / 2 < 59.7f / 2);    // under it, not beside it
    CHECK(!p[1].resting && p[1].bottom > 0 && p[1].yaw < 0);  // the portrait one, on a stand, turned
}

static void TestFlightSim() {
    using namespace luma::app::games;
    using luma::fx::Kind;
    CHECK(FindProfile("FlightSimulator.exe", "")->feed == Feed::FlightSimConnect);
    CHECK(FindProfile("", "Microsoft Flight Simulator (2020)")->feed == Feed::FlightSimConnect);
    CHECK(FindProfile("FlightSimulator2024.exe", "")->kind == ProfileKind::BuiltIn);
    int n = 0;
    FlightSimVars(&n);
    CHECK(n == 10);
    // SimConnect's values, in FlightSimVars() order.
    double v[10] = {0, 0, 0, 0, 1, 0, 1, 40, 100, 1};  // flying by day, beacon on
    FlightSimLighting f;
    CHECK(!f.Active(1000));
    f.OnState(FlightSimFromValues(v, 10), 1000);
    CHECK(f.Active(1500) && !f.Active(1000 + FlightSimLighting::kStaleMs));
    CHECK(f.Current().kind == Kind::Beat && f.Current().color1 == (Rgb{255, 0, 0}));  // beacon flashes red
    v[5] = 1;  // strobes on: white flashes win
    f.OnState(FlightSimFromValues(v, 10), 1000);
    CHECK(f.Current().color1 == (Rgb{255, 255, 255}) && f.Current().color2 == (Rgb{60, 150, 255}));
    v[7] = 5;  // 5 of 100 gallons left
    f.OnState(FlightSimFromValues(v, 10), 1000);
    CHECK(f.Current().kind == Kind::Breathing);
    v[1] = 1;  // stall warning above all
    f.OnState(FlightSimFromValues(v, 10), 1000);
    CHECK(f.Current().kind == Kind::Strobe && f.Current().color1 == (Rgb{255, 0, 0}));
    double parked[10] = {1, 0, 0, 0, 0, 0, 0, 40, 100, 3};  // on the ground at night, engines off
    f.OnState(FlightSimFromValues(parked, 10), 1000);
    CHECK(f.Current().kind == Kind::Static && f.Current().color1 == Scale(Rgb{20, 30, 110}, 0.35));
    f.OnSimRunning(false);  // back in the menus
    CHECK(!f.Active(1100));
}

static void TestDcs() {
    using namespace luma::app::games;
    using luma::fx::Kind;
    CHECK(FindProfile("DCS.exe", "")->feed == Feed::DcsExport);
    DcsLighting d;
    Json j;
    CHECK(Json::Parse(R"({"alive":1,"tod":43200,"g":1.0,"agl":1500,"gear":0,"rpm":85,"mw":0})", &j));
    CHECK(d.OnPacket(j, 1000) && d.Active(1200) && !d.Active(1000 + DcsLighting::kStaleMs));
    CHECK(d.Current().kind == Kind::Static && d.Current().color1 == (Rgb{60, 150, 255}));  // noon sky
    Json::Parse(R"({"alive":1,"tod":43200,"g":1.0,"agl":300,"gear":1,"rpm":70,"mw":0})", &j);
    d.OnPacket(j, 1000);
    CHECK(d.Current().color1 == (Rgb{0, 255, 60}));  // three greens
    Json::Parse(R"({"alive":1,"tod":43200,"g":9.2,"agl":300,"gear":1,"rpm":70,"mw":0})", &j);
    d.OnPacket(j, 1000);
    CHECK(d.Current().color1 == (Rgb{255, 0, 0}));  // pulling 9 G
    Json::Parse(R"({"alive":1,"tod":43200,"g":9.2,"agl":300,"gear":1,"rpm":70,"mw":1})", &j);
    d.OnPacket(j, 1000);
    CHECK(d.Current().kind == Kind::Strobe);  // master warning above all
    Json::Parse(R"({"alive":0,"tod":0,"g":1,"agl":0,"gear":0,"rpm":0,"mw":0})", &j);
    d.OnPacket(j, 1000);
    CHECK(!d.Active(1100));  // spectating / no aircraft
    Json::Parse(R"({"stop":1})", &j);
    d.OnPacket(j, 1000);
    CHECK(!d.Active(1100));

    // Export.lua: our line is added once, at the end, and taken out again leaving the rest.
    const std::string srs = "local dcsSr=require('lfs');dofile(dcsSr.writedir()..[[Mods\\Services\\DCS-SRS\\x.lua]])\r\n";
    const std::string with = DcsExportWithHook(srs);
    CHECK(DcsHasHook(with) && with.rfind(srs, 0) == 0);
    CHECK(DcsExportWithHook(with) == with);
    CHECK(DcsExportWithoutHook(with) == srs);
    CHECK(DcsHasHook(DcsExportWithHook("")) && DcsExportWithoutHook(DcsExportWithHook("")).empty());
    CHECK(DcsHasHook(DcsExportWithHook("-- no newline")));
    const std::string script = DcsScriptText();
    CHECK(script.find("LuaExportAfterNextFrame") != std::string::npos && script.find("49717") != std::string::npos);
    CHECK(script.find("prevAfter") != std::string::npos);  // chains the scripts loaded before it
}

static void TestForza() {
    using namespace luma::app::games;
    using luma::fx::Kind;
    ForzaLighting f;
    auto packet = [](int32_t raceOn, float maxRpm, float idle, float rpm) {
        std::vector<uint8_t> p(324, 0);  // a Dash packet; only the start is read
        std::memcpy(&p[0], &raceOn, 4);
        std::memcpy(&p[8], &maxRpm, 4);
        std::memcpy(&p[12], &idle, 4);
        std::memcpy(&p[16], &rpm, 4);
        return p;
    };
    CHECK(!f.OnPacket(packet(1, 8000, 800, 800).data(), 10, 1000));  // too short
    auto p = packet(0, 8000, 800, 3000);
    CHECK(f.OnPacket(p.data(), p.size(), 1000) && !f.Active(1000));  // menus / paused
    p = packet(1, 8000, 800, 1500);
    f.OnPacket(p.data(), p.size(), 2000);
    CHECK((f.Active(2000) && f.Current().kind == Kind::Static && f.Current().color1 == luma::Rgb{0, 90, 255}));
    p = packet(1, 8000, 800, 7000);  // 86 %: yellow into red
    f.OnPacket(p.data(), p.size(), 3000);
    CHECK(f.Current().color1.r == 255 && f.Current().color1.g < 220);
    p = packet(1, 8000, 800, 7950);  // the limiter
    f.OnPacket(p.data(), p.size(), 4000);
    CHECK(f.Current().kind == Kind::Strobe);
    CHECK(!f.Active(4000 + ForzaLighting::kStaleMs + 1));
    CHECK(FindProfile("ForzaHorizon5.exe", "")->feed == Feed::ForzaDataOut);
    CHECK(FindProfile("League of Legends.exe", "")->feed == Feed::LeagueLiveClient);
    CHECK(FindProfile("dota2.exe", "")->feed == Feed::Dota2Gsi && FindProfile("x.exe", "Forza Motorsport") != nullptr);
}

static void TestWarThunder() {
    using namespace luma::app::games;
    using luma::fx::Kind;
    WarThunderLighting wt;
    wt.OnIndicators(J(R"({"valid":false})"), 1000);
    CHECK(!wt.Active(1000));
    wt.OnIndicators(J(R"({"valid":true,"army":"tank","crew_total":4,"crew_current":4})"), 2000);
    CHECK(wt.Active(2000));
    CHECK((wt.Current(2000).color1 == luma::Rgb{0, 255, 0}));
    wt.OnIndicators(J(R"({"valid":true,"army":"tank","crew_total":4,"crew_current":1})"), 3000);
    CHECK(wt.Current(3000).kind == Kind::Strobe);      // crew lost
    CHECK(wt.Current(4000).kind == Kind::Breathing);   // then low crew
    CHECK(wt.Current(4000).color1.r == 255);
    wt.OnIndicators(J(R"({"valid":true,"army":"air"})"), 5000);
    wt.OnState(J(R"({"valid":true,"throttle 1, %":110,"Mfuel, kg":500,"Mfuel0, kg":1000})"), 5000);
    CHECK(wt.Current(5000).kind == Kind::Twinkle);     // war emergency power
    wt.OnState(J(R"({"valid":true,"throttle 1, %":80,"Mfuel, kg":50,"Mfuel0, kg":1000})"), 5100);
    CHECK(wt.Current(5100).kind == Kind::Breathing);   // low fuel
    CHECK(!wt.Active(5000 + WarThunderLighting::kStaleMs + 1));
}

static void TestScreenColors() {
    using namespace luma::app::games;
    // Left half pure-ish red, right half blue; plus some black.
    std::vector<uint8_t> px;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x) {
            if (x < 2) px.insert(px.end(), {10, 10, 200, 255});
            else px.insert(px.end(), {220, 20, 10, 255});
        }
    ScreenColors c = SummarizeScreen(px, 4, 2);
    CHECK(c.left.r > 150 && c.left.b < 60);
    CHECK(c.right.b > 150 && c.right.r < 60);
    std::vector<uint8_t> black(4 * 4 * 4, 0);
    c = SummarizeScreen(black, 4, 4);
    CHECK(c.left.IsBlack() && c.right.IsBlack());
}

static void TestSmbios() {
    using namespace luma::app::sensors;
    std::vector<uint8_t> t;
    auto add = [&](std::vector<uint8_t> formatted, std::vector<std::string> strings) {
        t.insert(t.end(), formatted.begin(), formatted.end());
        if (strings.empty()) t.insert(t.end(), {0, 0});
        for (const auto& str : strings) {
            t.insert(t.end(), str.begin(), str.end());
            t.push_back(0);
        }
        if (!strings.empty()) t.push_back(0);
    };
    // Type 0 BIOS: vendor=1, version=2, date=3 (length 0x12).
    std::vector<uint8_t> bios(0x12, 0);
    bios[0] = 0; bios[1] = 0x12; bios[4] = 1; bios[5] = 2; bios[8] = 3;
    add(bios, {"American Megatrends Inc.", "3405", "02/01/2024"});
    // Type 2 baseboard: maker=1, product=2.
    std::vector<uint8_t> board(0x08, 0);
    board[0] = 2; board[1] = 0x08; board[4] = 1; board[5] = 2;
    add(board, {"ASUSTeK COMPUTER INC.", "ROG STRIX B550-F GAMING (WI-FI)  "});
    // Type 4 processor: version string index at 0x10.
    std::vector<uint8_t> cpu(0x1A, 0);
    cpu[0] = 4; cpu[1] = 0x1A; cpu[0x10] = 1;
    add(cpu, {"AMD Ryzen 7 5800X 8-Core Processor"});
    // Type 17 memory: size 8192 MB, locator=1, speed 3600, maker=2, part=3, configured 3600.
    std::vector<uint8_t> mem(0x28, 0);
    mem[0] = 17; mem[1] = 0x28; mem[0x0C] = 0x00; mem[0x0D] = 0x20; mem[0x10] = 1;
    mem[0x15] = 0x10; mem[0x16] = 0x0E; mem[0x17] = 2; mem[0x1A] = 3; mem[0x20] = 0x10; mem[0x21] = 0x0E;
    add(mem, {"DIMM_A2", "Kingston", "KF3600C17D4/8GX"});
    // An empty slot (size 0), a structure without strings, then end-of-table.
    std::vector<uint8_t> empty(0x28, 0);
    empty[0] = 17; empty[1] = 0x28;
    add(empty, {});
    add({127, 4, 0, 0}, {});
    std::vector<uint8_t> raw{0, 3, 3, 0, 0, 0, 0, 0};
    const uint32_t len = static_cast<uint32_t>(t.size());
    raw[4] = len & 0xFF; raw[5] = (len >> 8) & 0xFF;
    raw.insert(raw.end(), t.begin(), t.end());
    const SmbiosInfo info = ParseSmbios(raw);
    CHECK(info.biosVersion == "3405" && info.biosVendor == "American Megatrends Inc.");
    CHECK(info.boardName == "ROG STRIX B550-F GAMING (WI-FI)" && info.boardMaker == "ASUSTeK COMPUTER INC.");
    CHECK(info.cpu == "AMD Ryzen 7 5800X 8-Core Processor");
    CHECK(info.memory.size() == 1);
    CHECK(info.memory[0].sizeMb == 8192 && info.memory[0].speedMts == 3600 && info.memory[0].part == "KF3600C17D4/8GX");
    CHECK(ParseSmbios({1, 2, 3}).boardName.empty());  // garbage in, nothing out
}

static void TestLhm() {
    using namespace luma::app::sensors;
    const char* text = R"json({"id":0,"Text":"Sensor","Children":[{"id":1,"Text":"PC","ImageURL":"images_icon/computer.png","Children":[
      {"id":2,"Text":"ASUS ROG STRIX B550-F GAMING","ImageURL":"images_icon/mainboard.png","Children":[
        {"id":3,"Text":"Nuvoton NCT6798D","ImageURL":"images_icon/chip.png","Children":[
          {"id":4,"Text":"Temperatures","ImageURL":"images_icon/temperature.png","Children":[
            {"id":5,"Text":"Motherboard","Value":"34,0 °C","Type":"Temperature","Children":[]}]},
          {"id":6,"Text":"Fans","ImageURL":"images_icon/fan.png","Children":[
            {"id":7,"Text":"CPU Fan","Value":"1180 RPM","Children":[]},
            {"id":8,"Text":"Chassis Fan #1","Value":"845 RPM","Type":"Fan","Children":[]}]}]}]},
      {"id":9,"Text":"AMD Ryzen 7 5800X","ImageURL":"images_icon/cpu.png","Children":[
        {"id":10,"Text":"Temperatures","ImageURL":"images_icon/temperature.png","Children":[
          {"id":11,"Text":"Core (Tctl/Tdie)","Value":"55.3 °C","Type":"Temperature","Children":[]}]},
        {"id":12,"Text":"Load","ImageURL":"images_icon/load.png","Children":[
          {"id":13,"Text":"CPU Total","Value":"12.5 %","Type":"Load","Children":[]}]}]}]}]})json";
    luma::Json j;
    CHECK(luma::Json::Parse(text, &j));
    const auto s = ParseLhm(j);
    CHECK(s.size() == 5);
    CHECK(s[0].kind == HardwareKind::Board && s[0].type == SensorType::Temperature && s[0].value == 34.0);
    CHECK(s[1].type == SensorType::Fan && s[1].value == 1180 && s[1].unit == "RPM" && s[1].hardware == "Nuvoton NCT6798D");
    CHECK(s[3].kind == HardwareKind::Cpu && s[3].name == "Core (Tctl/Tdie)" && s[3].value > 55.2 && s[3].value < 55.4);
    CHECK(s[4].type == SensorType::Load && s[4].value == 12.5);
    CHECK(PickSensor(s, HardwareKind::Cpu, SensorType::Temperature, {"Tctl", "Package"})->value > 55);
    CHECK(PickSensor(s, HardwareKind::Board, SensorType::Fan, {"Chassis"})->value == 845);
    CHECK(PickSensor(s, HardwareKind::Gpu, SensorType::Temperature, {}) == nullptr);
}

static void TestFriendlyNames() {
    using namespace luma::app::sensors;
    CHECK(FriendlyBoard("ASUSTeK COMPUTER INC.", "ROG STRIX B550-F GAMING") == "ASUS ROG STRIX B550-F GAMING");
    CHECK(FriendlyBoard("", "B550 AORUS") == "B550 AORUS");
    CHECK(FriendlyMemory("Kingston", "HX436C17FB3/8") == "HyperX FURY");
    CHECK(FriendlyMemory("Kingston", "KF3600C17D4/8GX") == "Kingston FURY");
    CHECK(FriendlyMemory("Corsair", "CMW16GX4M2C3200C16") == "Corsair Vengeance RGB");
    CHECK(FriendlyMemory("Samsung", "M378A1K43CB2-CTD") == "Samsung M378A1K43CB2-CTD");
    CHECK(FriendlyCpu("AMD Ryzen 7 5800X 8-Core Processor") == "AMD Ryzen 7 5800X");
    CHECK(FriendlyCpu("Intel(R) Core(TM) i7-12700K CPU @ 3.60GHz") == "Intel Core i7-12700K");
    CHECK(FriendlyGpu("NVIDIA GeForce RTX 3080") == "NVIDIA GeForce RTX 3080");
}

static void TestAzoth() {
    using namespace luma::app::azoth;
    // Byte-for-byte what Armoury Crate sent for red (FF 00 2C) at 100 % brightness.
    const Report r = StaticColor(luma::Rgb{0xFF, 0x00, 0x2C});
    const uint8_t expected[] = {0x00, 0x51, 0x2C, 0x00, 0x00, 0xFF, 0x64, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x2C};
    CHECK(std::memcmp(r.data(), expected, sizeof expected) == 0);
    for (size_t i = sizeof expected; i < r.size(); ++i) CHECK(r[i] == 0);
    CHECK(!IsSave(r));
    Report save{};
    save[1] = 0x50;
    save[2] = 0x55;
    CHECK(IsSave(save));
    // Wireless, through the Omni receiver: frame 1977 of the capture (green, 100 %).
    const Report w = StaticColor(luma::Rgb{0x00, 0xFF, 0x00}, Link::Wireless);
    const uint8_t wexpected[] = {0x02, 0x51, 0x2C, 0x00, 0x00, 0xFF, 0x64, 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0x00};
    CHECK(std::memcmp(w.data(), wexpected, sizeof wexpected) == 0);
    for (size_t i = sizeof wexpected; i < w.size(); ++i) CHECK(w[i] == 0);
    CHECK(ReportSize(Link::Wireless) == 64 && ReportSize(Link::Wired) == 65);
    CHECK(Product(Link::Wireless) == 0x1ACE && !IsSave(w));
    save[0] = 0x02;
    CHECK(IsSave(save));

    // The effects, byte for byte as Armoury Crate sent them (data bytes, report ID 0 first).
    auto same = [](const Report& r, std::initializer_list<uint8_t> data) {
        size_t i = 1;
        for (uint8_t b : data)
            if (r[i++] != b) return false;
        for (; i < r.size(); ++i)
            if (r[i] != 0) return false;
        return r[0] == 0;
    };
    CHECK(same(StaticColor(luma::Rgb{0xFF, 0x40, 0x00}), {0x51, 0x2C, 0x00, 0x00, 0xFF, 0x64, 0x00, 0xFF, 0xFF, 0xFF, 0x40}));
    auto one = Packets(Breathing(luma::Rgb{0xFF, 0xFF, 0xFF}, 0x1E, 0x32));
    CHECK(one.size() == 1 && same(one[0], {0x51, 0x2C, 0x01, 0x00, 0x1E, 0x32, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    one = Packets(Breathing(luma::Rgb{0xFF, 0x00, 0x00}, luma::Rgb{0xFF, 0x00, 0xFF}, 0x2A, 0x64));
    CHECK(same(one[0], {0x51, 0x2C, 0x01, 0x00, 0x2A, 0x64, 0x10, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF}));
    CHECK(same(Packets(ColorCycle(0x1F, 0x64))[0], {0x51, 0x2C, 0x02, 0x00, 0x1F, 0x64, 0x00, 0xFF, 0xFF}));
    CHECK(same(Packets(Simple(Mode::Reactive, luma::Rgb{0xFF, 0xFF, 0xFF}, true, 0x0F, 0x32))[0],
               {0x51, 0x2C, 0x03, 0x00, 0x0F, 0x32, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    const auto wave = Packets(RainbowWave(0x3E, 0x32));
    CHECK(wave.size() == 3);
    CHECK(same(wave[0], {0x51, 0x2C, 0x04, 0x02, 0x3E, 0x32, 0x00, 0x00, 0x02, 0x07, 0x0E, 0xF5, 0x00, 0xFF, 0x1D, 0x00, 0x06, 0xFF}));
    CHECK(same(wave[1], {0x51, 0x2C, 0x04, 0x01, 0x2B, 0x00, 0xFA, 0xFF, 0x39, 0x01, 0xFF, 0x00, 0x48, 0xFF, 0xF6, 0x00, 0x56, 0xFF, 0x78, 0x07}));
    CHECK(same(wave[2], {0x51, 0x2C, 0x04, 0x00, 0x64, 0xFF, 0x00, 0x0D}));
    const auto ripple = Packets(RainbowRipple(0x64, 0x32));
    CHECK(ripple.size() == 3);
    CHECK(same(ripple[0], {0x51, 0x2C, 0x05, 0x02, 0x64, 0x32, 0x01, 0xFF, 0x02, 0x07, 0x0E, 0xF5, 0x00, 0xFF, 0x1D, 0x00, 0x06, 0xFF}));
    CHECK(same(ripple[2], {0x51, 0x2C, 0x05, 0x00, 0x64, 0xFF, 0x00, 0x0D}));
    const luma::Rgb sand[6] = {{0xFF, 0xF6, 0x00}, {0x01, 0xFF, 0x00}, {0x00, 0xFA, 0xFF},
                               {0x00, 0x06, 0xFF}, {0xF5, 0x00, 0xFF}, {0xFF, 0x00, 0x0D}};
    const auto qs = Packets(Quicksand(sand, 0x0F, 0x32));
    CHECK(qs.size() == 2);
    CHECK(same(qs[0], {0x51, 0x2C, 0x07, 0x01, 0x0F, 0x32, 0x01, 0x02, 0xFF, 0xFF, 0xF6, 0x00, 0x01, 0xFF, 0x00, 0x00, 0xFA, 0xFF}));
    CHECK(same(qs[1], {0x51, 0x2C, 0x07, 0x00, 0x00, 0x06, 0xFF, 0xF5, 0x00, 0xFF, 0xFF, 0x00, 0x0D}));
    for (const Report& r : wave) CHECK(!IsSave(r));
    CHECK(Packets(ColorCycle(0x1F, 0x64), Link::Wireless)[0][0] == 0x02);
}

static void TestLogitechHidpp() {
    CHECK(std::strcmp(luma::app::hidpp::DeviceTypeName(3), "Mouse") == 0);
    CHECK(std::strcmp(luma::app::hidpp::DeviceTypeName(0), "Keyboard") == 0);
    CHECK(std::strcmp(luma::app::hidpp::DeviceTypeName(-1), "Device") == 0);
    using namespace luma::app::hidpp;
    // The G502 X Plus's list (cluster 0): 0 off, 1 fixed, 2 breathing, 3 cycle; RGB effects at index 09.
    Layout l;
    l.fixed = 1;
    l.breathing = 2;
    l.cycle = 3;
    l.colorWave = true;
    // Byte for byte what G HUB sent from byte 4 on (its software ID is 0B, ours 0A).
    auto same = [](const Report& r, std::initializer_list<uint8_t> params) {
        if (r[0] != 0x11 || r[1] != 0x01 || r[2] != 0x09 || r[3] != 0x1A) return false;
        size_t i = 4;
        for (uint8_t b : params)
            if (r[i++] != b) return false;
        for (; i < r.size(); ++i)
            if (r[i] != 0) return false;
        return true;
    };
    Effect e;
    e.kind = Kind::Fixed;
    e.color = luma::Rgb{0x00, 0xFF, 0xFF};
    CHECK(same(SetEffect(1, 9, l, e), {0x00, 0x01, 0x00, 0xFF, 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0x01}));
    e.kind = Kind::Breathing;
    e.periodMs = 5000;
    CHECK(same(SetEffect(1, 9, l, e), {0x00, 0x02, 0x00, 0xFF, 0xFF, 0x13, 0x88, 0x00, 0x64, 0, 0, 0, 0x01}));
    e.kind = Kind::Cycle;
    CHECK(same(SetEffect(1, 9, l, e), {0x00, 0x03, 0, 0, 0, 0, 0, 0x13, 0x88, 0x64, 0, 0, 0x01}));
    e.kind = Kind::ColorWave;
    CHECK(same(SetEffect(1, 9, l, e), {0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0x88, 0x01, 0x64, 0x13, 0x01}));
    e.periodMs = 5500;
    e.intensity = 83;
    CHECK(same(SetEffect(1, 9, l, e), {0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0x7C, 0x01, 0x53, 0x15, 0x01}));

    // Replies.
    const Report req = Request(1, 9, 1, {0x00});
    Report ok{0x11, 0x01, 0x09, 0x1A};
    Report err{0x11, 0x01, 0xFF, 0x09, 0x1A, 0x05};
    CHECK(Answers(ok, req) && !Refuses(ok, req));
    CHECK(Refuses(err, req) && !Answers(err, req));

    // LumaBridge's effects -> the mouse's.
    luma::fx::Params p;
    p.kind = luma::fx::Kind::RainbowWave;
    p.speed = 0.2;
    auto m = ForEffect(p, l);
    CHECK(m && m->kind == Kind::ColorWave && m->periodMs == 5000);
    p.saturation = 0.5;  // pastel: the mouse can't
    CHECK(!ForEffect(p, l));
    p.saturation = 1;
    p.kind = luma::fx::Kind::ColorCycle;
    p.speed = 5;  // faster than the mouse goes
    m = ForEffect(p, l);
    CHECK(m && m->kind == Kind::Cycle && m->periodMs == 1000);
    p.kind = luma::fx::Kind::Breathing;
    p.color1 = luma::Rgb{1, 2, 3};
    m = ForEffect(p, l);
    CHECK(m && m->kind == Kind::Breathing && m->color == (luma::Rgb{1, 2, 3}));
    p.speed = 0;
    CHECK(!ForEffect(p, l));
    p.speed = 0.5;
    p.kind = luma::fx::Kind::Static;
    CHECK(!ForEffect(p, l));
    p.kind = luma::fx::Kind::RainbowWave;
    l.colorWave = false;  // not known to work on this mouse
    CHECK(!ForEffect(p, l));

    // Per-key: effect 4 of cluster FF (ID 0013) on, then frames of zone R G B.
    CHECK(!l.perKey());
    l.perKeyEffect = 4;
    l.perKeyFeature = 0x0A;
    l.strip.assign(kG502XPlusStrip.begin(), kG502XPlusStrip.end());
    CHECK(l.perKey());
    CHECK(same(StartPerKey(1, 9, l), {0xFF, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01}));
    luma::Rgb c[8];
    for (int i = 0; i < 8; ++i) c[i] = luma::Rgb{static_cast<uint8_t>(i), 0x10, 0x20};
    const auto f = PerKeyFrame(1, l, c);
    CHECK(f.size() == 3);
    const uint8_t first[] = {0x11, 0x01, 0x0A, 0x1A, 3, 0, 0x10, 0x20, 4, 1, 0x10, 0x20, 8, 2, 0x10, 0x20, 7, 3, 0x10, 0x20};
    CHECK(std::memcmp(f[0].data(), first, 20) == 0);
    CHECK(f[1][4] == 6 && f[1][8] == 5 && f[1][12] == 2 && f[1][16] == 1 && f[1][17] == 7);
    CHECK(f[2][2] == 0x0A && f[2][3] == 0x7A && f[2][4] == 0);
    // Six LEDs: the second report is padded with its first LED again.
    l.strip.resize(6);
    const auto g = PerKeyFrame(1, l, c);
    CHECK(g.size() == 3 && g[1][4] == 6 && g[1][8] == 5 && g[1][12] == 6 && g[1][16] == 6 && g[1][17] == 4);
}

static void TestDeviceLighting() {
    using namespace luma::app;
    DeviceLighting d;
    d.own = true;
    d.look.effect = luma::fx::Kind::Gradient;
    d.look.color1 = luma::Rgb{0xFF, 0x1A, 0x00};
    d.look.color2 = luma::Rgb{0x00, 0x80, 0xFF};
    d.look.speedHz = 0.25f;
    d.look.hueStart = 170;
    d.look.hueSpan = 90;
    d.look.saturation = 0.45f;
    d.look.spread = 2;
    d.look.reverse = true;
    d.brightness = 0.4f;
    const std::string enc = EncodeDevice(d);
    CHECK(enc == "1|5|FF1A00|0080FF|0.25|170|90|0.45|2|1|0.4|0|0");
    DeviceLighting back;
    CHECK(DecodeDevice(enc, &back) && back.own && back.look == d.look && back.brightness == 0.4f);
    // 0.7.0 wrote no brightness: full.
    CHECK(DecodeDevice("1|5|FF1A00|0080FF|0.25|170|90|0.45|2|1", &back) && back.look == d.look && back.brightness == 1.f);
    CHECK(DecodeDevice("1|5|FF1A00|0080FF|0.25|170|90|0.45|2|1|7", &back) && back.brightness == 1.f);  // out of range
    CHECK(!DecodeDevice("", &back) && !DecodeDevice("1|5|FF1A00", &back));
    CHECK(!DecodeDevice("1|99|FF1A00|0080FF|0.25|170|90|0.45|2|1", &back));  // no such effect
    CHECK(!DecodeDevice("1|5|FF1A0|0080FF|0.25|170|90|0.45|2|1", &back));    // bad color
    CHECK(!DecodeDevice("1|5|FF1A00|0080FF|25|170|90|0.45|2|1", &back));     // speed out of range
    CHECK(DecodeDevice("0|0|000000|FFFFFF|0.5|0|360|1|9|0", &back) && !back.own && back.look.spread == 1);
    // The device's direction (12th field); older lines leave it the same way as the others.
    DeviceLighting flipped = d;
    flipped.reverse = true;
    CHECK(DecodeDevice(EncodeDevice(flipped), &back) && back.reverse && back.brightness == 0.4f);
    CHECK(DecodeDevice("1|5|FF1A00|0080FF|0.25|170|90|0.45|2|1|0.5", &back) && !back.reverse);
    DeviceLighting handed = d;
    handed.native = true;
    CHECK(DecodeDevice(EncodeDevice(handed), &back) && back.native && !back.reverse);
    CHECK(DecodeDevice("1|5|FF1A00|0080FF|0.25|170|90|0.45|2|1|0.5|1", &back) && back.reverse && !back.native);
    CHECK(std::string(NativeApp(device::kMouse)) == "G HUB" && std::string(NativeApp(device::kFans)) == "Armoury Crate");

    // Static never moves; the rest keep their speed.
    Look l;
    CHECK(ToParams(l).speed == 0);
    l.effect = luma::fx::Kind::Comet;
    l.reverse = true;
    const luma::fx::Params p = ToParams(l);
    CHECK(p.kind == luma::fx::Kind::Comet && p.speed == 0.5 && p.reverse && p.color1 == l.color1);

    // The canvas.
    Spot s;
    CHECK(DecodeSpot(EncodeSpot(Spot{0.25f, 0.75f}), &s) && s == (Spot{0.25f, 0.75f}));
    CHECK(DecodeSpot("1.5,-2", &s) && s == (Spot{1, 0}));
    CHECK(!DecodeSpot("0.5", &s));
    CHECK(DecodeSpot(EncodeSpot(Spot{0.25f, 0.75f, 45}), &s) && s.angle == 45 && s.y == 0.75f);  // turned on the desk
    CHECK(EncodeSpot(Spot{0.25f, 0.75f}).find(',', 7) == std::string::npos);                    // unturned: as before
    CHECK(FanItem(3) == "fan3");
    const Spot f0 = DefaultSpot("fan0"), f3 = DefaultSpot("fan3");
    CHECK(f3.x > f0.x && f3.y == f0.y);  // the fourth fan starts a second column
    for (const char* id : device::All()) CHECK(device::Name(id)[0] != 0);
}

static void TestSharedClock() {
    using namespace luma::fx;
    // Seconds into an effect: from its shared start when it has one, else the device's own.
    Params p;
    CHECK(Seconds(p, 5000, 3000) == 2.0);
    p.epoch = 1000;
    CHECK(Seconds(p, 5000, 3000) == 4.0 && Seconds(p, 500, 3000) == 0.0);
    // A beat: 1 a second at first, 5 a second after 40 s; beats counted continuously.
    Params b;
    b.kind = Kind::Beat;
    b.color1 = luma::Rgb{255, 0, 0};
    b.color2 = luma::Rgb{0, 0, 0};
    b.speed = 1;
    b.speedEnd = 5;
    b.rampSeconds = 40;
    CHECK(std::fabs(BeatPhase(b, 1.0) - (1.0 + 4.0 / 4800.0)) < 1e-9);  // ~1 beat after 1 s
    CHECK(std::fabs(BeatPhase(b, 40.0) - (40.0 + 4.0 * 40.0 / 3.0)) < 1e-9);
    CHECK(std::fabs(BeatPhase(b, 41.0) - BeatPhase(b, 40.0) - 5.0) < 1e-9);  // then 5 a second
    // The tempo rises smoothly: beats per second between 1 and 5, never backwards.
    double last = 0;
    for (double t = 0.5; t <= 45; t += 0.5) {
        const double ph = BeatPhase(b, t);
        CHECK(ph > last);
        last = ph;
    }
    // Each beat starts with a full flash that fades out within 30 % of it.
    CHECK((Render(b, 0.0, 0, 1) == luma::Rgb{255, 0, 0}));
    CHECK((Render(b, 0.5, 0, 1) == luma::Rgb{0, 0, 0}));
    CHECK(Render(b, 0.1, 0, 1).r > 0 && Render(b, 0.1, 0, 1).r < 255);
    // Two devices asking at the same moment see the same thing, whenever they got the effect.
    Params e = b;
    e.epoch = 10000;
    CHECK(Render(e, Seconds(e, 12345, 10500), 0, 1) == Render(e, Seconds(e, 12345, 11900), 0, 1));
}

static void TestDeviceSleep() {
    namespace sl = luma::app::sleep;
    // Awake until the timeout, then a fade over kFadeMs, then dark; asleep once the dark frame is out.
    CHECK(sl::Level(1000, 0, 60000) == 1.0 && sl::Level(60000, 0, 60000) == 1.0);
    CHECK(std::fabs(sl::Level(60000 + sl::kFadeMs / 2, 0, 60000) - 0.5) < 1e-9);
    CHECK(sl::Level(60000 + sl::kFadeMs, 0, 60000) == 0.0 && sl::Level(999999, 0, 60000) == 0.0);
    CHECK(!sl::Asleep(60000 + sl::kFadeMs, 0, 60000) && sl::Asleep(60000 + sl::kFadeMs + sl::kQuietMs, 0, 60000));
    CHECK(sl::Level(999999, 0, 0) == 1.0 && !sl::Asleep(999999, 0, 0));  // timeout 0: never sleeps
    CHECK(sl::Level(10, 50, 1000) == 1.0);                                 // input after "now": awake
    // G HUB's answer, as it came from a real G HUB (ghub-probe --scan).
    const std::string on = R"({
 "msgId": "probe-46",
 "verb": "GET",
 "path": "/lighting/turn_off_for_inactivity",
 "origin": "backend",
 "result": {
  "code": "SUCCESS",
  "what": ""
 },
 "payload": {
  "@type": "type.googleapis.com/logi.protocol.util.Enable",
  "enabled": true
 }
})";
    CHECK(sl::ParseGHubEnabled(on) == std::optional<bool>(true));
    std::string off = on;
    off.replace(off.find("true"), 4, "false");
    CHECK(sl::ParseGHubEnabled(off) == std::optional<bool>(false));
    CHECK(!sl::ParseGHubEnabled(R"({"path": "/lighting/turn_off_for_inactivity", "result": {"code": "INVALID_ARG"}})"));
    CHECK(!sl::ParseGHubEnabled(R"({"path": "/other", "result": {"code": "SUCCESS"}, "payload": {"enabled": true}})"));
}

static void TestLampArray() {
    namespace la = luma::app::lamparray;
    // Levels: 256 or more pass through; fewer are scaled; 0 / 1 level is on or off.
    CHECK(la::Level(200, 256) == 200 && la::Level(255, 255) == 254 && la::Level(0, 255) == 0);
    CHECK(la::Level(255, 16) == 15 && la::Level(128, 16) == 8 && la::Level(9, 1) == 1 && la::Level(0, 0) == 0);
    // Columns by X: left edge 0, right edge the last, a single lamp 0.
    CHECK(la::Column(1000, 1000, 5000, 32) == 0 && la::Column(5000, 1000, 5000, 32) == 31);
    CHECK(la::Column(3000, 1000, 5000, 32) == 16 && la::Column(7, 7, 7, 32) == 0 && la::Column(500, 1000, 5000, 32) == 0);
    // A frame of 10 lamps in reports of 4: 4 + 4 + 2, in order.
    std::vector<std::pair<uint16_t, luma::Rgb>> lamps;
    for (uint16_t i = 0; i < 10; ++i) lamps.emplace_back(i, luma::Rgb{static_cast<uint8_t>(i), 0, 0});
    const auto b = la::Batches(lamps, 4);
    CHECK(b.size() == 3 && b[0].size() == 4 && b[2].size() == 2 && b[1][0].first == 4 && b[2][1].first == 9);
    CHECK(la::Batches(lamps, 0).empty() && la::Batches({}, 4).empty());
    CHECK(std::strcmp(la::KindName(1), "Keyboard") == 0 && std::strcmp(la::KindName(99), "Device") == 0);
}

static void TestOpenRgb() {
    namespace o = luma::app::openrgb;
    // A packet: "ORGB", device, ID, size, data.
    const o::Bytes pk = o::Packet(3, o::kUpdateLeds, {1, 2});
    CHECK(pk.size() == 18 && pk[0] == 'O' && pk[3] == 'B' && pk[4] == 3 && pk[8] == 0x1A && pk[9] == 0x04 && pk[12] == 2);
    auto h = o::ParseHeader(pk.data(), pk.size());
    CHECK(h && h->device == 3 && h->id == o::kUpdateLeds && h->size == 2);
    CHECK(!o::ParseHeader(pk.data(), 10));
    // UpdateLEDs: size of everything, count, colors (R | G << 8 | B << 16).
    const o::Bytes up = o::UpdateLeds({luma::Rgb{1, 2, 3}, luma::Rgb{255, 0, 0}});
    CHECK(up.size() == 14 && up[0] == 14 && up[4] == 2 && up[6] == 1 && up[7] == 2 && up[8] == 3 && up[10] == 255);

    // A controller as OpenRGB describes it (protocol 1): two modes (the second active), a
    // linear zone of 3 LEDs and a matrix zone of 2, 5 LEDs.
    o::Bytes c;
    auto str = [&](const std::string& t) {
        o::PutU16(&c, static_cast<uint16_t>(t.size() + 1));
        c.insert(c.end(), t.begin(), t.end());
        c.push_back(0);
    };
    auto mode = [&](const std::string& name, uint16_t colors) {
        str(name);
        for (int i = 0; i < 9; ++i) o::PutU32(&c, static_cast<uint32_t>(i));
        o::PutU16(&c, colors);
        for (uint16_t i = 0; i < colors; ++i) o::PutU32(&c, 0x00FF00);
    };
    o::PutU32(&c, 0);  // size, filled in below
    o::PutU32(&c, 6);  // mouse
    str("Corsair Harpoon RGB");
    str("Corsair");
    str("desc");
    str("1.0");
    str("serial");
    str("HID: /dev/x");
    o::PutU16(&c, 2);
    o::PutU32(&c, 1);  // active mode
    mode("Direct", 0);
    const size_t activeAt = c.size();
    mode("Static", 1);
    const size_t activeEnd = c.size();
    o::PutU16(&c, 2);  // zones
    str("Logo");
    o::PutU32(&c, 1);
    o::PutU32(&c, 3);
    o::PutU32(&c, 3);
    o::PutU32(&c, 3);
    o::PutU16(&c, 0);  // no matrix
    str("Keys");
    o::PutU32(&c, 2);
    o::PutU32(&c, 2);
    o::PutU32(&c, 2);
    o::PutU32(&c, 2);
    o::PutU16(&c, 16);  // matrix: height 1, width 2, map of 2
    for (uint32_t v : {1u, 2u, 0u, 1u}) o::PutU32(&c, v);
    o::PutU16(&c, 5);  // LEDs
    for (int i = 0; i < 5; ++i) {
        str("LED " + std::to_string(i));
        o::PutU32(&c, static_cast<uint32_t>(i));
    }
    o::PutU16(&c, 5);  // colors
    for (int i = 0; i < 5; ++i) o::PutU32(&c, 0);
    const uint32_t size = static_cast<uint32_t>(c.size());
    for (int i = 0; i < 4; ++i) c[static_cast<size_t>(i)] = static_cast<uint8_t>(size >> (8 * i));

    auto ctl = o::ParseController(c.data(), c.size(), 1);
    CHECK(ctl && ctl->type == 6 && ctl->name == "Corsair Harpoon RGB" && ctl->vendor == "Corsair");
    CHECK(ctl->location == "HID: /dev/x" && ctl->zones.size() == 2 && ctl->zones[0].leds == 3 && ctl->zones[1].leds == 2);
    CHECK(ctl->leds == 5 && ctl->activeMode == 1);
    CHECK(ctl->activeModeData == o::Bytes(c.begin() + static_cast<long>(activeAt), c.begin() + static_cast<long>(activeEnd)));
    const o::Bytes um = o::UpdateMode(1, ctl->activeModeData);
    CHECK(um.size() == 8 + ctl->activeModeData.size() && um[0] == um.size() && um[4] == 1);
    // Cut short: not parsed (skipped, not a crash).
    CHECK(!o::ParseController(c.data(), c.size() - 3, 1));
    CHECK(!o::ParseController(c.data(), 3, 1));
    CHECK(std::strcmp(o::TypeName(6), "Mouse") == 0 && std::strcmp(o::TypeName(99), "Device") == 0);
}

static void TestDeviceCatalog() {
    namespace c = luma::app::catalog;
    CHECK(std::string(c::RgbBrand(0x1B1C, 0x1234)) == "Corsair");
    CHECK(std::string(c::RgbBrand(0x048D, 0x5702)) == "Gigabyte RGB Fusion");
    CHECK(c::RgbBrand(0x048D, 0x1234) == nullptr);  // ITE makes more than RGB controllers
    CHECK(c::RgbBrand(0x0951, 0x1666) == nullptr);  // a Kingston USB stick isn't RGB gear
    const auto brands = c::RgbBrands({{0x1B1C, 1}, {0x1B1C, 2}, {0x046D, 3}, {0x1E71, 4}, {0x8086, 5}}, {0x046D});
    CHECK(brands.size() == 2 && brands[0] == "Corsair" && brands[1] == "NZXT");
    CHECK(c::RgbMemory("Corsair", "CMW16GX4M2D3600C18") == "Corsair Vengeance RGB");
    CHECK(c::RgbMemory("Corsair", "CMK16GX4M2B3200C16").empty());  // Vengeance LPX: no RGB
    CHECK(c::RgbMemory("G Skill Intl", "F4-3600C16-8GTZNC") == "G.Skill Trident Z Neo");
    CHECK(c::RgbMemory("G Skill Intl", "F4-3200C16-8GTZR") == "G.Skill Trident Z RGB");
    CHECK(c::RgbMemory("G Skill Intl", "F4-3200C16-8GVKB").empty());
    CHECK(c::RgbMemory("Kingston", "KF3600C17D4/8GX").empty());  // FURY: setup_hardware.h
}

static void TestSetupPlan() {
    using namespace luma::app::setup;
    uint16_t vid = 0, pid = 0;
    CHECK(ParseVidPid(L"USB\\VID_046D&PID_C547\\5&1A2B", &vid, &pid) && vid == 0x046D && pid == 0xC547);
    CHECK(ParseVidPid(L"usb\\vid_0b05&pid_1ace&mi_02", &vid, &pid) && vid == 0x0B05 && pid == 0x1ACE);
    CHECK(!ParseVidPid(L"ROOT\\SYSTEM\\0000", &vid, &pid));
    CHECK(!ParseVidPid(L"USB\\VID_04", &vid, &pid));

    Answers a;
    AddUsbDevice(&a, 0x046D, 0xC547);
    AddUsbDevice(&a, 0x0B05, 0x1ACE);  // the Azoth's Omni receiver
    AddUsbDevice(&a, 0x0B05, 0x19AF);  // an Aura controller
    AddUsbDevice(&a, 0x8086, 0x0001);  // not an RGB vendor
    CHECK(a.has(Brand::Logitech) && a.has(Brand::Azoth) && a.has(Brand::Asus));
    CHECK(!a.has(Brand::Razer) && !a.has(Brand::Corsair));

    // Everything on, except hardware the user doesn't have and vendor runtimes in use.
    CHECK(DefaultOn(Conn::Lightsync, a) && DefaultOn(Conn::GameSense, a) && DefaultOn(Conn::Helper, a));
    CHECK(DefaultOn(Conn::Chroma, a) && DefaultOn(Conn::AlienFx, a) && DefaultOn(Conn::Handback, a));
    CHECK(DefaultOn(Conn::LogitechDevices, a) && DefaultOn(Conn::Azoth, a));
    CHECK(!DefaultOn(Conn::RamLighting, a) && *OffReason(Conn::RamLighting, a));
    a.set(App::Synapse);
    CHECK(!DefaultOn(Conn::Chroma, a) && std::strstr(OffReason(Conn::Chroma, a), "Synapse"));
    CHECK(*OffReason(Conn::Lightsync, a) == 0);
    for (int i = 0; i < kConns; ++i) CHECK(Info(static_cast<Conn>(i)).install == (*Info(static_cast<Conn>(i)).integration != 0));
}

static void TestSetupHardware() {
    using namespace luma::app;
    CHECK(SlotIndex("DIMM_A1") == 0 && SlotIndex("DIMM_A2") == 1 && SlotIndex("DIMM_B1") == 2 && SlotIndex("DIMM_B2") == 3);
    CHECK(SlotIndex("A2") == 1 && SlotIndex("B1") == 2);
    CHECK(SlotIndex("ChannelA-DIMM0") == 0 && SlotIndex("ChannelB-DIMM1") == 3);
    CHECK(SlotIndex("P0 CHANNEL A / DIMM 1") == 0 && SlotIndex("P0 CHANNEL B / DIMM 2") == 3);
    CHECK(SlotIndex("BANK 0") == -1 && SlotIndex("") == -1);
    // AMD boards: the channel in the bank locator, "DIMM 0" / "DIMM 1" counted from zero.
    CHECK(SlotIndex("P0 CHANNEL B / DIMM 0", true) == 2 && SlotIndex("P0 CHANNEL A / DIMM 1", true) == 1);
    {
        luma::app::sensors::SmbiosInfo amd;
        for (auto [slot, bank] : {std::pair<const char*, const char*>{"DIMM 0", "P0 CHANNEL A"}, {"DIMM 1", "P0 CHANNEL A"},
                                  {"DIMM 0", "P0 CHANNEL B"}}) {
            luma::app::sensors::MemoryModule m;
            m.slot = slot;
            m.bank = bank;
            m.manufacturer = "Kingston";
            m.part = "KF3600C17D4/8GX";
            amd.memory.push_back(m);
        }
        const SetupHardware three = DetectSetup(amd);  // three sticks in the first three slots
        CHECK(three.slotsKnown && three.slots[0] && three.slots[1] && three.slots[2] && !three.slots[3]);
    }
    // Channels numbered from zero.
    CHECK(SlotIndex("P0_Node0_Channel0_Dimm0") == 0 && SlotIndex("P0_Node0_Channel1_Dimm1") == 3);
    // Slots numbered in a row, without channels.
    for (auto names : {std::vector<const char*>{"DIMM 1", "DIMM 2", "DIMM 3"}, std::vector<const char*>{"DIMM0", "DIMM1", "DIMM2"}}) {
        luma::app::sensors::SmbiosInfo row;
        for (const char* n : names) {
            luma::app::sensors::MemoryModule m;
            m.slot = n;
            row.memory.push_back(m);
        }
        const SetupHardware r = DetectSetup(row);
        CHECK(r.slotsKnown && r.slots[0] && r.slots[1] && r.slots[2] && !r.slots[3]);
    }
    {
        luma::app::sensors::SmbiosInfo same;  // two sticks both called "DIMM 0": not understood
        for (int i = 0; i < 2; ++i) {
            luma::app::sensors::MemoryModule m;
            m.slot = "DIMM 0";
            same.memory.push_back(m);
        }
        CHECK(!DetectSetup(same).slotsKnown);
    }

    luma::app::sensors::SmbiosInfo info;
    info.boardMaker = "ASUSTeK COMPUTER INC.";
    info.boardName = "ROG STRIX B550-F GAMING (WI-FI)";
    for (const char* slot : {"DIMM_A1", "DIMM_A2", "DIMM_B2"}) {
        luma::app::sensors::MemoryModule m;
        m.slot = slot;
        m.manufacturer = "Kingston";
        m.part = "KF3600C17D4/8GX";
        info.memory.push_back(m);
    }
    SetupHardware h = DetectSetup(info);
    CHECK(h.board == BoardStyle::RogStrix && h.boardName.find("ROG STRIX B550-F") != std::string::npos);
    CHECK(h.ram == RamStyle::KingstonFury && h.ramName == "Kingston FURY");
    CHECK(h.sticks == 3 && h.slotsKnown && h.slots[0] && h.slots[1] && !h.slots[2] && h.slots[3]);

    info.boardName = "TUF GAMING X570-PLUS";
    info.memory[0].part = "HX436C17FB3A/8";
    info.memory[2].slot = "BANK 3";  // can't tell: fall back to the count
    h = DetectSetup(info);
    CHECK(h.board == BoardStyle::Tuf && h.ram == RamStyle::HyperXFury && !h.slotsKnown && h.sticks == 3);
    info.boardName = "B450M DS3H";
    CHECK(DetectSetup(info).board == BoardStyle::Generic);
    CHECK(DetectSetup(luma::app::sensors::SmbiosInfo{}).sticks == 0);
    const auto g = GuessSlots(2);
    CHECK(!g[0] && g[1] && !g[2] && g[3]);
}

static void TestAzothKeys() {
    using namespace luma::app::azoth;
    // 82 keys, each LED once, each where the map put it (column * 8 + row).
    const auto& keys = IsoKeys();
    CHECK(keys.size() == 82);
    bool seen[128] = {};
    for (const Key& k : keys) {
        CHECK(k.led >= 0 && k.led < 128 && !seen[k.led]);
        seen[k.led] = true;
        CHECK(k.x >= 0 && k.x + k.w <= 16.001f && k.y >= 0 && k.y + k.h <= 6);
        if (k.led != 107) CHECK(static_cast<int>(k.y) == k.led % 8);  // the row
    }
    auto at = [&](int led) {
        for (const Key& k : keys)
            if (k.led == led) return k;
        return Key{-1, 0, 0, 0, 0};
    };
    CHECK(at(0).x == 0 && at(53).w == 6.25f && at(107).h == 2);
    CHECK(at(121).x == 15 && at(125).x == 15);  // the right-hand column
    CHECK(EffectColumn(at(0)) == 1 && EffectColumn(at(125)) == kColumns - 1);
    // Rows end at 16 units.
    for (int row = 1; row <= 5; ++row) {
        float right = 0;
        for (const Key& k : keys)
            if (static_cast<int>(k.y) == row || (k.led == 107 && row == 2)) right = std::max(right, k.x + k.w);
        CHECK(right == 16);
    }

    // The per-key command: 15 keys a report.
    std::vector<KeyColor> kc;
    for (int i = 0; i < 17; ++i) kc.push_back({static_cast<uint8_t>(i), luma::Rgb{static_cast<uint8_t>(i), 2, 3}});
    const auto r = KeyColors(kc);
    CHECK(r.size() == 2);
    const uint8_t head[] = {0x00, 0xC0, 0x81, 15, 0x00, 0, 0, 2, 3, 1, 1, 2, 3};
    CHECK(std::memcmp(r[0].data(), head, sizeof head) == 0);
    CHECK(r[1][3] == 2 && r[1][5] == 15 && r[1][9] == 16 && r[1][13] == 0);
    CHECK(KeyColors(kc, Link::Wireless)[0][0] == 0x02 && !IsSave(r[0]));
    // Through the Omni receiver a report is 64 bytes: 14 keys, so every key's blue fits
    // (with 15, the 15th key - 1, Q, S, X, space - lost its blue).
    const auto w = KeyColors(kc, Link::Wireless);
    CHECK(KeysPerReport(Link::Wired) == 15 && KeysPerReport(Link::Wireless) == 14);
    CHECK(w.size() == 2 && w[0][3] == 14 && w[1][3] == 3 && w[1][5] == 14);
    for (const auto& rep : w)
        for (size_t k = 0; k < rep[3]; ++k) CHECK(8 + k * 4 < ReportSize(Link::Wireless) && rep[8 + k * 4] == 3);
    luma::fx::Params p;
    p.kind = luma::fx::Kind::Static;
    p.color1 = luma::Rgb{200, 100, 50};
    const auto colors = RenderKeys(p, 0, 0.5);
    CHECK(colors.size() == keys.size() && colors[0] == (luma::Rgb{100, 50, 25}));
}

static void TestHyperXRam() {
    using namespace luma::app::ram;
    std::array<luma::Rgb, kMaxLeds> colors{};
    for (int i = 0; i < kMaxLeds; ++i) colors[static_cast<size_t>(i)] = luma::Rgb{static_cast<uint8_t>(i), 0x80, 0xFF};
    // Two sticks in SPD slots 1 and 3 (A2 / B2 on most boards).
    const auto w = Frame(0b1010, colors);
    CHECK(w.size() == 1 + 2 * (1 + 5 * 4) + 2);
    CHECK(w.front().reg == 0xE1 && w.front().value == 0x01);
    CHECK(w[1].reg == 0xE5 && w[1].value == 0x21);
    CHECK(w[2].reg == 0x41 && w[2].value == 5);        // slot 1, LED 0, red = colors[5].r
    CHECK(w[3].reg == 0x42 && w[3].value == 0x80);     // green
    CHECK(w[4].reg == 0x43 && w[4].value == 0xFF);     // blue
    CHECK(w[5].reg == 0x51 && w[5].value == 0x64);     // brightness 100
    CHECK(w[22].reg == 0xE5);                          // slot 3 next
    CHECK(w[23].reg == 0xA1 && w[23].value == 15);
    CHECK(w[w.size() - 2].reg == 0xE1 && w[w.size() - 2].value == 0x02);
    CHECK(w.back().reg == 0xE1 && w.back().value == 0x03);
    for (const auto& x : w) CHECK(IsAllowed(x.reg));
    // Everything else stays off limits.
    int allowed = 0;
    for (int r = 0; r < 256; ++r) allowed += IsAllowed(static_cast<uint8_t>(r));
    CHECK(allowed == 5 + kSlots * kLedsPerStick * 4);
    CHECK(!IsAllowed(0x00) && !IsAllowed(0x20) && !IsAllowed(0xE4) && !IsAllowed(0xE5 + 1) && !IsAllowed(0xFF));
    const auto rainbow = OwnRainbow();  // letting go: the sticks' own rainbow
    CHECK(rainbow.size() == 6 && rainbow[1].reg == 0xE3 && rainbow[1].value == 0x05);
    CHECK(rainbow[2].value == 0x07 && rainbow[3].value == 0xD0 && rainbow.back().value == 0x03);
    for (const auto& x : rainbow) CHECK(IsAllowed(x.reg));
    CHECK(Frame(0, colors).size() == 3);  // no sticks: nothing but the update / apply
}

static void TestHwSensors() {
    using namespace luma::app::hw;
    // Tctl: 0x2D0 eighths = 90.0 C; with range select, 49 C lower.
    CHECK(std::fabs(TctlCelsius(0x2D0u << 21) - 90.0) < 1e-9);
    CHECK(std::fabs(TctlCelsius((0x2D0u << 21) | 0x80000u) - 41.0) < 1e-9);
    CHECK(std::fabs(TctlCelsius((0x2D0u << 21) | 0x30000u) - 41.0) < 1e-9);
    // Package power from the energy counter: Ryzen's unit is usually 2^-16 J (0x0A1003 has
    // 0x10 in bits 12:8); 3,276,800 counts in 1 s is 50 W. The counter wraps.
    const double unit = EnergyUnitJoules(0x0A1003);
    CHECK(std::fabs(unit - 1.0 / 65536) < 1e-12);
    CHECK(std::fabs(PackageWatts(1000, 1000 + 3276800, unit, 1.0) - 50.0) < 1e-9);
    CHECK(std::fabs(PackageWatts(0xFFFF0000u, 0x00310000u, unit, 1.0) - 50.0) < 1e-9);  // wrapped
    CHECK(PackageWatts(0, 100, unit, 0.01) < 0);                                        // too soon
    // Intel: TjMax 100 (IA32_TEMPERATURE_TARGET bits 23:16), 38 below it -> 62 °C.
    CHECK(IntelPackageCelsius(100ull << 16, 0x80000000ull | (38ull << 16)) == 62);
    CHECK(IntelPackageCelsius(100ull << 16, 38ull << 16) < 0);  // not valid
    CHECK(std::fabs(TctlCelsius((0x2D0u << 21) | 0x10000u) - 90.0) < 1e-9);  // one TJ_SEL bit: no shift
    // Fans: count 1350 = 1000 RPM; the idle / missing readings are 0.
    CHECK(std::fabs(FanRpm(1350 >> 5, 1350 & 0x1F) - 1000.0) < 1e-9);
    CHECK(FanRpm(0xFF, 0x1F) == 0);   // max count: stopped
    CHECK(FanRpm(0, 0x10) == 0);      // below the minimum count
    double t = 0;
    CHECK(TemperatureValid(42, &t) && t == 42);
    CHECK(!TemperatureValid(0x7F, &t) && !TemperatureValid(0x80, &t));  // 127 / -128: no sensor
    CHECK(std::string(NuvotonChip(0xD4, 0x2B)) == "NCT6798D");
    CHECK(NuvotonChip(0xD4, 0x99) == nullptr && NuvotonChip(0x87, 0x12) == nullptr);
}

static void TestIpc() {
    using namespace luma::ipc;
    Frame f = MakeFrame(FrameKind::Color, 42, 1, 2, 3, "A very long source name that must be truncated");
    Frame g;
    CHECK(ParseFrame(&f, sizeof f, &g));
    CHECK(g.pid == 42 && g.r == 1 && g.b == 3 && std::strlen(g.source) == sizeof g.source - 1);
    CHECK(!ParseFrame(&f, sizeof f - 1, &g));
    f.kind = static_cast<FrameKind>(9);
    CHECK(!ParseFrame(&f, sizeof f, &g));
}

int main() {
    TestPercent();
    TestAuraPacking();
    TestCorrection();
    TestHue();
    TestEffects();
    TestBitmap();
    TestColorRefs();
    TestState();
    TestChroma();
    TestJson();
    TestHttp();
    TestGameSense();
    TestAuraUsb();
    TestCoreProps();
    TestCorsairDevices();
    TestLightFx();
    TestSources();
    TestGameCatalog();
    TestGameProfiles();
    TestCs2();
    TestRocketLeague();
    TestWarThunder();
    TestScreenColors();
    TestSmbios();
    TestLhm();
    TestFriendlyNames();
    TestAzoth();
    TestAzothKeys();
    TestLogitechHidpp();
    TestDeviceLighting();
    TestSetupHardware();
    TestSetupPlan();
    TestOpenRgb();
    TestLampArray();
    TestDeviceSleep();
    TestSharedClock();
    TestDota2();
    TestLeague();
    TestForza();
    TestFlightSim();
    TestScene3d();
    TestPcLayout();
    TestDisplayLayout();
    TestAioCatalog();
    TestDcs();
    TestDeviceCatalog();
    TestHyperXRam();
    TestHwSensors();
    TestIpc();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all core tests passed\n");
    return EXIT_SUCCESS;
}
