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
    TestIpc();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all core tests passed\n");
    return EXIT_SUCCESS;
}
