// Unit tests for the platform-independent core. Builds on any OS:
//   g++ -std=c++17 -I src/core -I src/integrations/razer -I src/integrations/corsair -I src/integrations/alienware -I src/integrations/steelseries tests/test_core.cpp -o test_core && ./test_core
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
#include "war_thunder_lighting.h"
#include "screen_colors.h"
#include "smbios.h"
#include "lhm.h"
#include "friendly_names.h"
#include "azoth_protocol.h"
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
    CHECK((Render(p, 1.0 / 3.0, 0, 3) == Rgb{0, 255, 0}));  // rotated one step

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
    const double early = cs.Current(4000).speed, late = cs.Current(4000 + 39000).speed;
    CHECK(cs.Current(4000).kind == Kind::Strobe && late > early);  // the fuse speeds up
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
    CHECK(!rl.Active(11000));
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
