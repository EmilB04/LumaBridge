// Unit tests for the platform-independent core. Builds on any OS:
//   g++ -std=c++17 -I src/common -I src/chroma-emu tests/test_core.cpp -o test_core && ./test_core
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "color.h"
#include "lighting_state.h"
#include "chroma_translate.h"

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

int main() {
    TestPercent();
    TestAuraPacking();
    TestCorrection();
    TestBitmap();
    TestColorRefs();
    TestState();
    TestChroma();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all core tests passed\n");
    return EXIT_SUCCESS;
}
