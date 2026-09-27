#include "ui.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <functional>
#include <iterator>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "controller.h"
#include "game_feeds.h"
#include "game_profiles.h"
#include "friendly_names.h"
#include "system_monitor.h"
#include "effects.h"
#include "imgui.h"
#include "integrations.h"

namespace luma::app {
namespace {

// ---- Palette -------------------------------------------------------------------------

constexpr ImU32 Hex(unsigned rgb, unsigned a = 255) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}
ImVec4 V4(unsigned rgb, float a = 1.f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}

constexpr unsigned kBg = 0x0E1014;
constexpr unsigned kSidebar = 0x13161C;
constexpr unsigned kCard = 0x191D25;
constexpr unsigned kCardHover = 0x20252F;
constexpr unsigned kBorder = 0x262B36;
constexpr unsigned kText = 0xE7E9EF;
constexpr unsigned kMuted = 0x8A92A6;
constexpr unsigned kAccent = 0x7C6CFF;
constexpr unsigned kAccentHover = 0x9384FF;
constexpr unsigned kGreen = 0x3DDC97;
constexpr unsigned kAmber = 0xF5B84B;
constexpr unsigned kRed = 0xFF6B6B;

#ifndef LUMA_VERSION
#define LUMA_VERSION "dev"
#endif
constexpr const char* kVersionText = "v" LUMA_VERSION;

float g_scale = 1.f;  // DPI scale, set by ApplyTheme
float S() { return g_scale; }

Rgb FromV4(const float* f) {
    auto b = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    return Rgb{b(f[0]), b(f[1]), b(f[2])};
}

// Color actually visible right now on the first LED (the sidebar orb).
Rgb PreviewColor(const Controller::Output& o) {
    if (o.stopped) return Rgb{60, 64, 76};
    return fx::Render(o.fx, ImGui::GetTime(), 0, 1);
}

// ---- Widgets -------------------------------------------------------------------------

void Orb(ImVec2 center, float radius, Rgb c) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 5; i >= 1; --i)
        dl->AddCircleFilled(center, radius + i * radius * 0.18f, IM_COL32(c.r, c.g, c.b, 10 + (5 - i) * 6), 48);
    dl->AddCircleFilled(center, radius, IM_COL32(c.r, c.g, c.b, 255), 48);
    dl->AddCircle(center, radius, IM_COL32(255, 255, 255, 40), 48, 1.5f);
}

void Muted(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void Pill(const char* text, unsigned color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pad(10 * S(), 3 * S());
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 size(ts.x + pad.x * 2, ts.y + pad.y * 2);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Hex(color, 40), size.y / 2);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), Hex(color), text);
    ImGui::Dummy(size);
}

bool BeginCard(const char* id, float height = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(kCard));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * S());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * S(), 16 * S()));
    ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (height <= 0) flags |= ImGuiChildFlags_AutoResizeY;
    bool open = ImGui::BeginChild(id, ImVec2(0, height), flags);
    return open;
}

void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 6 * S()));
}

void CardTitle(const Fonts& f, const char* title) {
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2 * S()));
}

// Segmented control; returns true when the selection changed.
bool Segmented(const char* id, int* current, const char* const* labels, int count, float width = 0) {
    bool changed = false;
    ImGui::PushID(id);
    const float h = ImGui::GetFrameHeight() + 4 * S();
    const float w = width > 0 ? width : 0;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * S(), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h / 2);
    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine();
        const bool sel = *current == i;
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? V4(kAccent) : V4(kCardHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? V4(kAccentHover) : V4(kBorder));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, V4(kAccent));
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? V4(0xFFFFFF) : V4(kMuted));
        const float bw = w > 0 ? (w - 4 * S() * (count - 1)) / count
                               : ImGui::CalcTextSize(labels[i]).x + 32 * S();
        if (ImGui::Button(labels[i], ImVec2(bw, h)) && !sel) {
            *current = i;
            changed = true;
        }
        ImGui::PopStyleColor(4);
    }
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 2 * S()));  // breathing room before whatever follows
    return changed;
}

bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, V4(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V4(kAccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, V4(kAccent));
    ImGui::PushStyleColor(ImGuiCol_Text, V4(0xFFFFFF));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

bool Swatch(const char* id, Rgb c, float size, bool selected = false) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("sw", ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = size / 2;
    ImVec2 center(p.x + r, p.y + r);
    dl->AddCircleFilled(center, r - 2 * S(), IM_COL32(c.r, c.g, c.b, 255), 32);
    if (selected || ImGui::IsItemHovered())
        dl->AddCircle(center, r - 0.5f, selected ? Hex(0xFFFFFF) : Hex(0xFFFFFF, 90), 32, 2 * S());
    ImGui::PopID();
    return clicked;
}

// Slider with a label on its own line and the value on the right.
bool LabeledSlider(const char* label, float* v, float min, float max, const char* fmt) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    ImGui::PushID(label);
    bool changed = ImGui::SliderFloat("##s", v, min, max, fmt, ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
    return changed;
}

// ---- Effects -------------------------------------------------------------------------

struct EffectInfo {
    const char* name;
    const char* help;
    float minSpeed, maxSpeed;  // maxSpeed 0: no speed slider
    const char* speedFmt;
};

// Indexed by fx::Kind.
const EffectInfo kEffects[] = {
    {"Static", "One color on every LED.", 0, 0, ""},
    {"Breathing", "Fades your color in and out.", 0.1f, 3.f, "%.1f breaths per second"},
    {"Strobe", "Flashes your color on and off.", 0.1f, 10.f, "%.1f flashes per second"},
    {"Color cycle", "Every LED shows the same color, cycling through the spectrum.", 0, 0, ""},
    {"Rainbow wave", "The whole spectrum around each fan, turning.", 0.05f, 2.f, "%.2f turns per second"},
    {"Gradient", "Blends your two colors around each fan. Speed 0 keeps it still.", 0.f, 2.f,
     "%.2f turns per second"},
    {"Comet", "A comet of your color with a fading tail chases around, over the second color.", 0.1f, 3.f,
     "%.1f laps per second"},
    {"Twinkle", "Your color with sparkles of the second color.", 0.1f, 3.f, "%.1f per second"},
};

// Effect picker: two rows of four.
bool EffectGrid(fx::Kind* kind, float width) {
    const char* names[8];
    for (int i = 0; i < 8; ++i) names[i] = kEffects[i].name;
    int sel = static_cast<int>(*kind);
    int row1 = sel < 4 ? sel : -1, row2 = sel >= 4 ? sel - 4 : -1;
    bool changed = false;
    if (Segmented("fx1", &row1, names, 4, width)) {
        *kind = static_cast<fx::Kind>(row1);
        changed = true;
    }
    if (Segmented("fx2", &row2, names + 4, 4, width)) {
        *kind = static_cast<fx::Kind>(row2 + 4);
        changed = true;
    }
    return changed;
}

ImU32 Col(Rgb c, int a = 255) { return IM_COL32(c.r, c.g, c.b, a); }

// Draws the fans on the ARGB header as rings of LEDs, and the board's LEDs as a row, showing
// what `out` looks like right now.
void LightsPreview(const Controller::Output& out, const fx::FanLayout& layout, int boardLeds) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double t = ImGui::GetTime();
    std::vector<Rgb> fans, board;
    if (out.stopped) {
        fans.assign(static_cast<size_t>(layout.TotalLeds()), Rgb{50, 54, 64});
        board.assign(static_cast<size_t>(boardLeds), Rgb{50, 54, 64});
    } else {
        if (out.fanTest) fx::RenderFanTest(layout, &fans);
        else fx::RenderFans(out.fx, t, layout, &fans);
        if (out.fanTest) board.assign(static_cast<size_t>(boardLeds), Rgb{});
        else fx::RenderStrip(out.fx, t, boardLeds, &board);
    }

    const float avail = ImGui::GetContentRegionAvail().x;
    const int n = layout.Fans();
    const float gap = 14 * S();
    const float size = std::min(96 * S(), (avail - gap * (std::min(n, 6) - 1)) / std::min(n, 6));
    const int perRow = std::max(1, static_cast<int>((avail + gap) / (size + gap)));
    const int per = layout.LedsPerFan();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const int rows = (n + perRow - 1) / perRow;
    for (int fan = 0; fan < n; ++fan) {
        const float x = origin.x + (fan % perRow) * (size + gap);
        const float y = origin.y + (fan / perRow) * (size + gap);
        const ImVec2 c(x + size / 2, y + size / 2);
        const float ringR = size * 0.40f;
        dl->AddCircleFilled(c, size * 0.47f, Hex(0x0A0C10), 48);
        dl->AddCircleFilled(c, size * 0.16f, Hex(kCardHover), 32);  // hub
        const float dot = std::clamp(ringR * 3.14159f / per * 0.8f, 1.5f * S(), 6 * S());
        for (int i = 0; i < per; ++i) {
            const Rgb led = fans[static_cast<size_t>(fan * per + i)];
            // LED 0 at the top, clockwise.
            const float a = -1.5707963f + 6.2831853f * i / per;
            const ImVec2 pt(c.x + std::cos(a) * ringR, c.y + std::sin(a) * ringR);
            dl->AddCircleFilled(pt, dot * 2.2f, Col(led, 45), 16);  // glow
            dl->AddCircleFilled(pt, dot, Col(led), 16);
        }
    }
    ImGui::Dummy(ImVec2(avail, rows * size + (rows - 1) * gap));

    if (boardLeds > 0) {
        ImGui::Dummy(ImVec2(0, 4 * S()));
        Muted("Motherboard");
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float d = 12 * S();
        for (int i = 0; i < boardLeds; ++i) {
            const ImVec2 pt(p.x + d / 2 + i * (d + 8 * S()), p.y + d / 2 + 2 * S());
            dl->AddCircleFilled(pt, d, Col(board[static_cast<size_t>(i)], 45), 16);
            dl->AddCircleFilled(pt, d / 2, Col(board[static_cast<size_t>(i)]), 16);
        }
        ImGui::Dummy(ImVec2(avail, d + 4 * S()));
    }
}

int BoardLedCount(Controller& ctl) {
    for (const auto& d : ctl.devices())
        if (d.type == 0x00010000) return d.lightCount;
    return 5;
}

void PreviewCard(Controller& ctl, const Fonts& f) {
    BeginCard("preview");
    CardTitle(f, "Preview");
    LightsPreview(ctl.output(), ctl.config().argbFans, BoardLedCount(ctl));
    Muted("Fans on the ARGB header, as set up on the Devices page.");
    EndCard();
}

// ---- Icons (drawn with lines and shapes, so no icon font is needed) ------------------

enum class Icon { Lighting, Game, Cpu, Gpu, Memory, Fan, Temp, Board, Leds, Plug, Info, Mouse, Keyboard };

ImVec2 At(ImVec2 c, float dx, float dy) { return ImVec2(c.x + dx, c.y + dy); }

// `s`: icon size in pixels; `spin`: rotation for the fan icon (radians).
void DrawIcon(Icon icon, ImVec2 c, float s, ImU32 col, float spin = 0) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = s * 0.5f, t = std::max(1.2f, s * 0.085f);
    switch (icon) {
    case Icon::Lighting:
        dl->AddCircle(At(c, 0, -r * 0.18f), r * 0.55f, col, 24, t);
        dl->AddLine(At(c, -r * 0.24f, r * 0.52f), At(c, r * 0.24f, r * 0.52f), col, t);
        dl->AddLine(At(c, -r * 0.16f, r * 0.76f), At(c, r * 0.16f, r * 0.76f), col, t);
        break;
    case Icon::Game:
        dl->AddRect(At(c, -r * 0.92f, -r * 0.5f), At(c, r * 0.92f, r * 0.5f), col, r * 0.45f, 0, t);
        dl->AddLine(At(c, -r * 0.62f, 0), At(c, -r * 0.22f, 0), col, t);
        dl->AddLine(At(c, -r * 0.42f, -r * 0.2f), At(c, -r * 0.42f, r * 0.2f), col, t);
        dl->AddCircleFilled(At(c, r * 0.3f, r * 0.08f), r * 0.1f, col);
        dl->AddCircleFilled(At(c, r * 0.55f, -r * 0.12f), r * 0.1f, col);
        break;
    case Icon::Cpu: {
        const float h = r * 0.55f;
        dl->AddRect(At(c, -h, -h), At(c, h, h), col, r * 0.1f, 0, t);
        dl->AddRectFilled(At(c, -h * 0.45f, -h * 0.45f), At(c, h * 0.45f, h * 0.45f), col, r * 0.05f);
        for (int i = -1; i <= 1; ++i) {
            const float o = i * h * 0.55f;
            dl->AddLine(At(c, o, -h), At(c, o, -r * 0.9f), col, t);
            dl->AddLine(At(c, o, h), At(c, o, r * 0.9f), col, t);
            dl->AddLine(At(c, -h, o), At(c, -r * 0.9f, o), col, t);
            dl->AddLine(At(c, h, o), At(c, r * 0.9f, o), col, t);
        }
        break;
    }
    case Icon::Gpu:
        dl->AddRect(At(c, -r * 0.8f, -r * 0.5f), At(c, r * 0.95f, r * 0.5f), col, r * 0.1f, 0, t);
        dl->AddCircle(At(c, r * 0.25f, 0), r * 0.3f, col, 20, t);
        dl->AddLine(At(c, -r * 0.8f, -r * 0.5f), At(c, -r * 0.8f, r * 0.85f), col, t);
        dl->AddLine(At(c, -r * 0.45f, r * 0.5f), At(c, -r * 0.45f, r * 0.72f), col, t);
        dl->AddLine(At(c, r * 0.6f, r * 0.5f), At(c, r * 0.6f, r * 0.72f), col, t);
        break;
    case Icon::Memory:
        dl->AddRect(At(c, -r * 0.95f, -r * 0.42f), At(c, r * 0.95f, r * 0.3f), col, r * 0.06f, 0, t);
        for (int i = 0; i < 4; ++i) {
            const float x = -r * 0.72f + i * r * 0.44f;
            dl->AddRectFilled(At(c, x, -r * 0.24f), At(c, x + r * 0.26f, r * 0.12f), col);
        }
        for (int i = 0; i < 7; ++i) {
            const float x = -r * 0.8f + i * r * 0.27f;
            dl->AddLine(At(c, x, r * 0.3f), At(c, x, r * 0.5f), col, t * 0.8f);
        }
        break;
    case Icon::Fan:
        dl->AddCircle(c, r * 0.92f, col, 28, t);
        for (int i = 0; i < 3; ++i) {
            const float a = spin + i * 2.0943951f;
            const ImVec2 p1 = At(c, std::cos(a) * r * 0.75f, std::sin(a) * r * 0.75f);
            const ImVec2 p2 = At(c, std::cos(a + 0.9f) * r * 0.55f, std::sin(a + 0.9f) * r * 0.55f);
            dl->AddTriangleFilled(c, p1, p2, col);
        }
        dl->AddCircleFilled(c, r * 0.18f, Hex(kCard));
        dl->AddCircle(c, r * 0.18f, col, 12, t);
        break;
    case Icon::Temp:
        dl->AddRect(At(c, -r * 0.18f, -r * 0.9f), At(c, r * 0.18f, r * 0.35f), col, r * 0.18f, 0, t);
        dl->AddCircleFilled(At(c, 0, r * 0.55f), r * 0.32f, col);
        dl->AddLine(At(c, 0, -r * 0.45f), At(c, 0, r * 0.4f), col, t * 1.4f);
        break;
    case Icon::Board:
        dl->AddRect(At(c, -r * 0.9f, -r * 0.9f), At(c, r * 0.9f, r * 0.9f), col, r * 0.12f, 0, t);
        dl->AddRectFilled(At(c, -r * 0.55f, -r * 0.55f), At(c, -r * 0.05f, -r * 0.05f), col);
        dl->AddLine(At(c, r * 0.15f, -r * 0.5f), At(c, r * 0.6f, -r * 0.5f), col, t);
        dl->AddLine(At(c, r * 0.15f, -r * 0.2f), At(c, r * 0.6f, -r * 0.2f), col, t);
        dl->AddLine(At(c, -r * 0.55f, r * 0.3f), At(c, r * 0.6f, r * 0.3f), col, t);
        dl->AddLine(At(c, -r * 0.55f, r * 0.58f), At(c, r * 0.2f, r * 0.58f), col, t);
        break;
    case Icon::Leds:
        for (int i = -1; i <= 1; ++i) dl->AddCircleFilled(At(c, i * r * 0.6f, 0), r * 0.22f, col);
        dl->AddLine(At(c, -r * 0.95f, r * 0.45f), At(c, r * 0.95f, r * 0.45f), col, t);
        break;
    case Icon::Plug:
        dl->AddRect(At(c, -r * 0.45f, -r * 0.25f), At(c, r * 0.45f, r * 0.35f), col, r * 0.12f, 0, t);
        dl->AddLine(At(c, -r * 0.2f, -r * 0.25f), At(c, -r * 0.2f, -r * 0.7f), col, t);
        dl->AddLine(At(c, r * 0.2f, -r * 0.25f), At(c, r * 0.2f, -r * 0.7f), col, t);
        dl->AddLine(At(c, 0, r * 0.35f), At(c, 0, r * 0.85f), col, t);
        break;
    case Icon::Mouse:
        dl->AddRect(At(c, -r * 0.5f, -r * 0.85f), At(c, r * 0.5f, r * 0.85f), col, r * 0.5f, 0, t);
        dl->AddLine(At(c, 0, -r * 0.85f), At(c, 0, -r * 0.2f), col, t);
        dl->AddLine(At(c, -r * 0.5f, -r * 0.2f), At(c, r * 0.5f, -r * 0.2f), col, t);
        break;
    case Icon::Keyboard:
        dl->AddRect(At(c, -r * 0.95f, -r * 0.5f), At(c, r * 0.95f, r * 0.5f), col, r * 0.12f, 0, t);
        for (int row = 0; row < 2; ++row)
            for (int k = 0; k < 5; ++k)
                dl->AddRectFilled(At(c, -r * 0.72f + k * r * 0.32f, -r * 0.3f + row * r * 0.28f),
                                  At(c, -r * 0.56f + k * r * 0.32f, -r * 0.16f + row * r * 0.28f), col);
        dl->AddLine(At(c, -r * 0.45f, r * 0.3f), At(c, r * 0.45f, r * 0.3f), col, t);
        break;
    case Icon::Info:
        dl->AddCircle(c, r * 0.88f, col, 24, t);
        dl->AddCircleFilled(At(c, 0, -r * 0.4f), r * 0.1f, col);
        dl->AddLine(At(c, 0, -r * 0.12f), At(c, 0, r * 0.5f), col, t * 1.2f);
        break;
    }
}

// An icon inline with text (advances the cursor like an item).
void IconItem(Icon icon, float size, ImU32 col, float spin = 0) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = std::max(size, ImGui::GetTextLineHeight());
    DrawIcon(icon, ImVec2(p.x + size / 2, p.y + h / 2), size, col, spin);
    ImGui::Dummy(ImVec2(size, h));
}

// ---- Dashboard ------------------------------------------------------------------------

struct DashCtx {
    HWND hwnd;
    Controller& ctl;
    Integrations& in;
    UiState& ui;
    const Fonts& f;
    const sensors::SystemSnapshot& snap;
};

unsigned TempColor(double c) { return c < 60 ? kGreen : c < 80 ? kAmber : kRed; }
unsigned LoadColor(double pct) { return pct < 70 ? kAccent : pct < 90 ? kAmber : kRed; }

void Bar(double fraction, unsigned color) {
    const float f = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, V4(color));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, V4(kCardHover));
    ImGui::ProgressBar(f, ImVec2(-1, 6 * S()), "");
    ImGui::PopStyleColor(2);
}

// A big number with a small unit, e.g. "42 %".
void Metric(const Fonts& f, const char* value, const char* unit, unsigned color = kText) {
    ImGui::PushFont(f.title);
    ImGui::PushStyleColor(ImGuiCol_Text, V4(color));
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (unit && *unit) {
        ImGui::SameLine(0, 4 * S());
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFontSize() * 0.35f);
        Muted("%s", unit);
    }
}

std::string Gb(uint64_t bytes) {
    char b[32];
    snprintf(b, sizeof b, "%.1f GB", bytes / 1073741824.0);
    return b;
}

void LhmHint() {
    Muted("Fan speeds and CPU / board temperatures come from LibreHardwareMonitor (free): run it, then "
          "Options > Remote Web Server > Run (port 8085).");
    if (ImGui::SmallButton("Get LibreHardwareMonitor"))
        ShellExecuteW(nullptr, L"open", L"https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases",
                      nullptr, nullptr, SW_SHOWNORMAL);
}

// Friendly name and icon for an Aura device.
std::string DeviceLabel(const AuraDeviceInfo& d, const sensors::SystemSnapshot& snap, const fx::FanLayout& fans,
                        Icon* icon) {
    if (d.type == 0x00010000) {
        *icon = Icon::Board;
        const std::string board = sensors::FriendlyBoard(snap.smbios.boardMaker, snap.smbios.boardName);
        return (board.empty() ? std::string("Motherboard") : board) + " lighting";
    }
    if (d.type == 0x00011000) {
        *icon = Icon::Fan;
        char b[96];
        snprintf(b, sizeof b, "Fans (%d x %d LEDs)", fans.Fans(), fans.LedsPerFan());
        return d.name.find(L"header ") != std::wstring::npos ? Utf8(d.name) + " - " + b : std::string(b);
    }
    *icon = Icon::Leds;
    return Utf8(d.name);
}

void WLighting(DashCtx& c) {
    const auto& out = c.ctl.output();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float orb = 18 * S();
    Orb(ImVec2(p.x + orb + 2 * S(), p.y + orb + 2 * S()), orb, PreviewColor(out));
    ImGui::Dummy(ImVec2(orb * 2 + 12 * S(), orb * 2 + 4 * S()));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(c.f.bold);
    ImGui::TextUnformatted(out.stopped ? "Armoury Crate" : c.ctl.prefs().mode == Mode::Auto ? "Auto" : "Manual");
    ImGui::PopFont();
    if (const uint64_t left = c.ctl.handbackMsLeft())
        Muted("Handing back... %d s", static_cast<int>((left + 999) / 1000));
    else
        Muted("%s", out.label.c_str());
    if (!out.stopped) Muted("Effect: %s", kEffects[static_cast<int>(out.fx.kind)].name);
    ImGui::EndGroup();
}

void WGame(DashCtx& c) {
    const auto& games = c.ctl.games();
    if (games.empty()) {
        Muted("No game running.");
        return;
    }
    for (const auto& g : games) {
        ImGui::PushFont(c.f.bold);
        ImGui::TextUnformatted(g.game.name.c_str());
        ImGui::PopFont();
        const bool active = g.support == games::Support::Active;
        const bool builtIn = g.profile && g.profile->kind == games::ProfileKind::BuiltIn;
        if (active) Pill(("Dynamic lighting - " + g.sdk).c_str(), kGreen);
        else if (c.ctl.screenColorsActive() && g.mode != GameMode::Idle && !builtIn) Pill("Screen colors", kAccent);
        else if (builtIn || games::SupportsLighting(g.support)) Pill("Waiting for its lighting", kAmber);
        else Pill("No dynamic lighting", kMuted);
    }
}

void WCpu(DashCtx& c) {
    const auto& s = c.snap;
    Muted("%s", sensors::FriendlyCpu(s.cpuName).c_str());
    char v[16] = "--";
    if (s.cpuLoad >= 0) snprintf(v, sizeof v, "%.0f", s.cpuLoad);
    Metric(c.f, v, "% load", s.cpuLoad >= 0 ? LoadColor(s.cpuLoad) : kMuted);
    Bar(s.cpuLoad / 100.0, LoadColor(s.cpuLoad));
    if (const auto* t = sensors::PickSensor(s.lhm, sensors::HardwareKind::Cpu, sensors::SensorType::Temperature,
                                            {"Tctl", "Package", "Core (Tdie)", "CPU"})) {
        ImGui::PushStyleColor(ImGuiCol_Text, V4(TempColor(t->value)));
        ImGui::Text("%.0f \xC2\xB0" "C", t->value);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        Muted("%s", t->name.c_str());
    } else if (!s.lhmConnected) {
        Muted("Temperature: needs LibreHardwareMonitor (see Fans).");
    }
    if (s.cpuThreads) Muted("%d threads", s.cpuThreads);
}

void WGpu(DashCtx& c) {
    const auto& s = c.snap;
    if (s.gpus.empty()) {
        Muted("No graphics card found.");
        return;
    }
    for (const auto& g : s.gpus) {
        ImGui::PushFont(c.f.bold);
        ImGui::TextUnformatted(sensors::FriendlyGpu(g.name).c_str());
        ImGui::PopFont();
        double load = g.load, temp = g.temp;
        if (load < 0)
            if (const auto* x = sensors::PickSensor(s.lhm, sensors::HardwareKind::Gpu, sensors::SensorType::Load, {"Core"}))
                load = x->value;
        if (temp < 0)
            if (const auto* x = sensors::PickSensor(s.lhm, sensors::HardwareKind::Gpu, sensors::SensorType::Temperature,
                                                    {"Core", "GPU"}))
                temp = x->value;
        if (load >= 0) {
            char v[16];
            snprintf(v, sizeof v, "%.0f", load);
            Metric(c.f, v, "% load", LoadColor(load));
            Bar(load / 100.0, LoadColor(load));
        }
        std::string line;
        char b[64];
        if (temp >= 0) {
            snprintf(b, sizeof b, "%.0f \xC2\xB0" "C", temp);
            ImGui::PushStyleColor(ImGuiCol_Text, V4(TempColor(temp)));
            ImGui::TextUnformatted(b);
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        if (g.fanPct >= 0) {
            snprintf(b, sizeof b, "fan %.0f %%  ", g.fanPct);
            line += b;
        }
        if (g.powerW >= 0) {
            snprintf(b, sizeof b, "%.0f W  ", g.powerW);
            line += b;
        }
        if (g.vramTotal) line += (g.vramUsed ? Gb(g.vramUsed) + " / " : std::string()) + Gb(g.vramTotal) + " VRAM";
        Muted("%s", line.c_str());
        if (load < 0 && temp < 0) Muted("Live stats: NVIDIA cards work out of the box; others need LibreHardwareMonitor.");
    }
}

void WMemory(DashCtx& c) {
    const auto& s = c.snap;
    if (s.memTotal) {
        const double used = static_cast<double>(s.memUsed) / static_cast<double>(s.memTotal);
        char v[16];
        snprintf(v, sizeof v, "%.0f", used * 100);
        Metric(c.f, v, "% used", LoadColor(used * 100));
        Bar(used, LoadColor(used * 100));
        Muted("%s of %s", Gb(s.memUsed).c_str(), Gb(s.memTotal).c_str());
    }
    // Group identical sticks: "2 x 8 GB HyperX FURY @ 3600 MT/s".
    std::vector<std::pair<std::string, int>> groups;
    for (const auto& m : s.smbios.memory) {
        char b[160];
        snprintf(b, sizeof b, "%u GB %s @ %u MT/s", m.sizeMb / 1024, sensors::FriendlyMemory(m.manufacturer, m.part).c_str(),
                 m.speedMts);
        bool found = false;
        for (auto& gr : groups)
            if (gr.first == b) found = ++gr.second > 0;
        if (!found) groups.push_back({b, 1});
    }
    for (const auto& gr : groups) Muted("%d x %s", gr.second, gr.first.c_str());
}

void WFans(DashCtx& c) {
    const auto& s = c.snap;
    int shown = 0, idle = 0;
    const float t = static_cast<float>(ImGui::GetTime());
    for (const auto& x : s.lhm) {
        if (x.type != sensors::SensorType::Fan) continue;
        if (x.value < 1) {
            ++idle;
            continue;
        }
        ImGui::PushID(shown++);
        IconItem(Icon::Fan, 18 * S(), Hex(kAccent), t * static_cast<float>(x.value) / 60.f * 0.35f);
        ImGui::SameLine();
        ImGui::Text("%.0f RPM", x.value);
        ImGui::SameLine();
        Muted("%s", x.name.c_str());
        ImGui::PopID();
    }
    for (const auto& g : s.gpus)
        if (g.fanPct >= 0) {
            IconItem(Icon::Fan, 18 * S(), Hex(kAccent), t * static_cast<float>(g.fanPct) * 0.05f);
            ImGui::SameLine();
            ImGui::Text("%.0f %%", g.fanPct);
            ImGui::SameLine();
            Muted("%s fan", sensors::FriendlyGpu(g.name).c_str());
            ++shown;
        }
    if (idle) Muted("%d header(s) with nothing connected / stopped", idle);
    if (!s.lhmConnected) LhmHint();
    else if (!shown && !idle) Muted("LibreHardwareMonitor reports no fans.");
}

void WTemps(DashCtx& c) {
    const auto& s = c.snap;
    int shown = 0;
    auto row = [&](const char* label, double v) {
        ImGui::PushStyleColor(ImGuiCol_Text, V4(TempColor(v)));
        ImGui::Text("%5.0f \xC2\xB0" "C", v);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        Muted("%s", label);
        ++shown;
    };
    if (const auto* x = sensors::PickSensor(s.lhm, sensors::HardwareKind::Cpu, sensors::SensorType::Temperature,
                                            {"Tctl", "Package", "CPU"}))
        row("CPU", x->value);
    for (const auto& g : s.gpus)
        if (g.temp >= 0) row(sensors::FriendlyGpu(g.name).c_str(), g.temp);
    for (const auto& x : s.lhm)
        if (x.type == sensors::SensorType::Temperature && x.value > 0 && x.value < 150 &&
            (x.kind == sensors::HardwareKind::Board || x.kind == sensors::HardwareKind::Storage ||
             x.kind == sensors::HardwareKind::Memory) && shown < 10)
            row((x.hardware.empty() ? x.name : x.name + " (" + x.hardware + ")").c_str(), x.value);
    if (!s.lhmConnected) LhmHint();
    else if (!shown) Muted("No temperatures reported.");
}

void WDevices(DashCtx& c) {
    const auto& devices = c.ctl.devices();
    if (devices.empty()) {
        Muted("No Aura devices found (Devices > Rescan).");
        return;
    }
    const auto& disabled = c.ctl.config().auraDisabledDevices;
    for (const auto& d : devices) {
        Icon icon;
        const std::string label = DeviceLabel(d, c.snap, c.ctl.config().argbFans, &icon);
        bool off = false;
        for (const auto& n : disabled) off |= _wcsicmp(n.c_str(), d.name.c_str()) == 0;
        IconItem(icon, 18 * S(), off ? Hex(kMuted) : Hex(kAccent));
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine();
        Muted("%d LEDs%s", d.lightCount, off ? " - off" : "");
    }
    Muted("AURA LED Controller (USB 0B05:1939)");
    if (c.ctl.prefs().logitechDevices) {
        const bool active = c.ctl.logitech().state() == LogitechOutput::State::Active;
        IconItem(Icon::Mouse, 18 * S(), active ? Hex(kAccent) : Hex(kMuted));
        ImGui::SameLine();
        ImGui::TextUnformatted("Logitech devices");
        ImGui::SameLine();
        Muted("%s", active ? "via G HUB" : "G HUB has them");
    }
    if (c.ctl.prefs().azothKeyboard) {
        const auto st = c.ctl.azoth().state();
        IconItem(Icon::Keyboard, 18 * S(), st == AzothOutput::State::Active ? Hex(kAccent) : Hex(kMuted));
        ImGui::SameLine();
        ImGui::TextUnformatted("ASUS ROG Azoth");
        ImGui::SameLine();
        Muted("%s", st == AzothOutput::State::Active     ? (c.ctl.azoth().wireless() ? "wireless" : "wired")
                    : st == AzothOutput::State::NotFound ? "not connected"
                                                         : "Armoury Crate's lighting");
    }
}

void EnsureIntegrations(Controller& ctl, Integrations& in, UiState& ui) {
    const auto& gs = ctl.gameSense();
    if (!ui.integrationsLoaded || in.TakeFinished()) {
        in.Refresh(gs.IsRunning(), gs.Port(), gs.CorePropsWritten(), gs.FoundSteelSeriesGG(), gs.ForwardPort(),
                   gs.ForwardOk());
        ui.integrationsLoaded = true;
    }
}

void WConnections(DashCtx& c) {
    EnsureIntegrations(c.ctl, c.in, c.ui);
    for (const auto& it : c.in.list()) {
        const bool on = it.state == IntegrationState::Active || it.state == IntegrationState::PerGame;
        const unsigned col = it.state == IntegrationState::Problem ? kRed : on ? kGreen : kMuted;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(ImGui::GetCursorScreenPos().x + 5 * S(), ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() / 2),
            4 * S(), Hex(col));
        ImGui::Dummy(ImVec2(12 * S(), ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextUnformatted(it.name.c_str());
    }
    const auto& feeds = c.ctl.feeds();
    const struct {
        const char* name;
        bool on;
    } built[] = {{"Counter-Strike 2 feed", feeds.Cs2Seen()},
                 {"Rocket League feed", feeds.RocketLeagueConnected()},
                 {"War Thunder feed", feeds.WarThunderSeen()}};
    for (const auto& b : built) {
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(ImGui::GetCursorScreenPos().x + 5 * S(), ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() / 2),
            4 * S(), Hex(b.on ? kGreen : kMuted));
        ImGui::Dummy(ImVec2(12 * S(), ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextUnformatted(b.name);
        if (!b.on) {
            ImGui::SameLine();
            Muted("idle");
        }
    }
}

void WSystem(DashCtx& c) {
    const auto& s = c.snap;
    const auto& b = s.smbios;
    auto line = [](const char* label, const std::string& v) {
        if (v.empty()) return;
        Muted("%s", label);
        ImGui::SameLine(90 * S());
        ImGui::TextUnformatted(v.c_str());
    };
    line("Board", sensors::FriendlyBoard(b.boardMaker, b.boardName));
    line("BIOS", b.biosVersion.empty() ? std::string() : b.biosVersion + (b.biosDate.empty() ? "" : " (" + b.biosDate + ")"));
    line("CPU", sensors::FriendlyCpu(s.cpuName));
    for (const auto& g : s.gpus) line("GPU", sensors::FriendlyGpu(g.name));
    if (s.memTotal) line("Memory", Gb(s.memTotal));
    line("LumaBridge", kVersionText);
}

struct DashWidget {
    const char* id;
    const char* title;
    Icon icon;
    void (*draw)(DashCtx&);
};

const DashWidget kWidgets[] = {
    {"lighting", "Lighting", Icon::Lighting, WLighting},     {"game", "Game", Icon::Game, WGame},
    {"cpu", "Processor", Icon::Cpu, WCpu},                   {"gpu", "Graphics", Icon::Gpu, WGpu},
    {"memory", "Memory", Icon::Memory, WMemory},             {"fans", "Fans", Icon::Fan, WFans},
    {"temps", "Temperatures", Icon::Temp, WTemps},           {"devices", "Aura devices", Icon::Leds, WDevices},
    {"connections", "Connections", Icon::Plug, WConnections}, {"system", "System", Icon::Info, WSystem},
};

const DashWidget* FindWidget(const std::string& id) {
    for (const auto& w : kWidgets)
        if (id == w.id) return &w;
    return nullptr;
}

void DashboardCustomize(Controller& ctl, UiState& ui, const Fonts& f) {
    BeginCard("dash-edit");
    CardTitle(f, "Customize the dashboard");
    Muted("Tick the cards to show, and use the arrows to change their order.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    auto& order = ctl.prefs().dashboard;
    // Visible cards in their order, then the hidden ones.
    std::vector<std::string> all = order;
    for (const auto& w : kWidgets)
        if (std::find(all.begin(), all.end(), w.id) == all.end()) all.push_back(w.id);
    bool changed = false;
    for (size_t i = 0; i < all.size(); ++i) {
        const DashWidget* w = FindWidget(all[i]);
        if (!w) continue;
        ImGui::PushID(w->id);
        auto pos = std::find(order.begin(), order.end(), all[i]);
        bool shown = pos != order.end();
        if (ImGui::Checkbox("##on", &shown)) {
            if (shown) order.push_back(all[i]);
            else order.erase(pos);
            changed = true;
        }
        ImGui::SameLine();
        IconItem(w->icon, 16 * S(), shown ? Hex(kAccent) : Hex(kMuted));
        ImGui::SameLine();
        ImGui::TextUnformatted(w->title);
        if (pos != order.end() && !changed) {
            const size_t k = static_cast<size_t>(pos - order.begin());
            ImGui::SameLine(260 * S());
            ImGui::BeginDisabled(k == 0);
            if (ImGui::ArrowButton("up", ImGuiDir_Up)) {
                std::swap(order[k], order[k - 1]);
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(k + 1 >= order.size());
            if (!changed && ImGui::ArrowButton("down", ImGuiDir_Down)) {
                std::swap(order[k], order[k + 1]);
                changed = true;
            }
            ImGui::EndDisabled();
        }
        ImGui::PopID();
        if (changed) break;  // the list changed under us; redraw next frame
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (ImGui::Button("Reset to default")) {
        order = DefaultDashboard();
        changed = true;
    }
    ImGui::SameLine();
    if (PrimaryButton("Done")) ui.dashEdit = false;
    if (changed) ctl.Changed();
    EndCard();
}

void DashboardPage(HWND hwnd, Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    const sensors::SystemSnapshot snap = ctl.monitor().Snapshot();
    Muted("The essentials at a glance. Cards can be hidden and reordered.");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 100 * S());
    if (ImGui::Button(ui.dashEdit ? "Close" : "Customize", ImVec2(100 * S(), 0))) ui.dashEdit = !ui.dashEdit;
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (ui.dashEdit) DashboardCustomize(ctl, ui, f);

    const auto& order = ctl.prefs().dashboard;
    if (order.empty()) {
        Muted("All cards are hidden. Click Customize to show some.");
        return;
    }
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = avail > 1000 * S() ? 3 : avail > 600 * S() ? 2 : 1;
    DashCtx c{hwnd, ctl, in, ui, f, snap};
    if (!ImGui::BeginTable("dash", cols, ImGuiTableFlags_SizingStretchSame)) return;
    for (const auto& id : order) {
        const DashWidget* w = FindWidget(id);
        if (!w) continue;
        ImGui::TableNextColumn();
        ImGui::PushID(w->id);
        BeginCard(w->id);
        IconItem(w->icon, 20 * S(), Hex(kAccent));
        ImGui::SameLine(0, 8 * S());
        ImGui::PushFont(f.bold);
        ImGui::TextUnformatted(w->title);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2 * S()));
        w->draw(c);
        EndCard();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// ---- Pages ---------------------------------------------------------------------------

void BrightnessCard(Controller& ctl, const Fonts& f) {
    BeginCard("brightness");
    CardTitle(f, "Brightness");
    float b = static_cast<float>(ctl.config().auraCorrection.brightness * 100.0);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##brightness", &b, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        ctl.config().auraCorrection.brightness = b / 100.0;
        ctl.Changed();
    }
    Muted("Applies to every Aura device, in both Auto and Manual mode.");
    EndCard();
}

void AutoPage(Controller& ctl, const Fonts& f) {
    BeginCard("now");
    CardTitle(f, "Dynamic lighting");
    Muted("Games drive your lights. When several are running, the one that changed color most recently wins.");
    ImGui::Dummy(ImVec2(0, 8 * S()));

    const auto& running = ctl.games();
    const auto others = ctl.unmatchedSources();
    if (running.empty() && others.empty()) {
        Pill("No game running", kMuted);
        ImGui::Dummy(ImVec2(0, 4 * S()));
        Muted("Games from Steam, Epic, EA, Ubisoft, GOG, Xbox, Riot and the ones Windows knows about "
              "show up here when they start. Those with Logitech, Razer, SteelSeries, Corsair or "
              "Alienware lighting drive your lights; set them up on the Integrations page.");
    }
    const uint64_t now = GetTickCount64();
    int row = 0;
    auto gameRow = [&](const std::string& name, Rgb color, bool lit, const std::string& detail, const char* pill,
                       unsigned pillColor) {
        ImGui::PushID(row++);
        Swatch("c", lit ? color : Rgb{46, 50, 60}, 28 * S(), lit && ctl.output().label.rfind(name, 0) == 0);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(f.bold);
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, 10 * S());
        Pill(pill, pillColor);
        Muted("%s", detail.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
    };
    const bool screenOn = ctl.screenColorsActive();
    for (const auto& g : running) {
        using games::Support;
        using games::ProfileKind;
        std::string detail = g.game.store;
        const char* pill = "No dynamic lighting";
        unsigned pillColor = kMuted;
        const bool builtIn = g.profile && g.profile->kind == ProfileKind::BuiltIn;
        switch (g.support) {
        case Support::Active:
            pill = "Dynamic lighting";
            pillColor = kGreen;
            detail += "  -  " + g.sdk;
            break;
        case Support::Known:
        case Support::SdkLoaded:
            pill = "Supports dynamic lighting";
            pillColor = kAmber;
            detail += "  -  " + (g.sdk.empty() ? std::string("has used lighting before") : g.sdk) +
                      "  -  waiting for its colors (usually once you're in a match)";
            break;
        default:
            if (builtIn) {
                pill = "Supports dynamic lighting";
                pillColor = kAmber;
                detail += "  -  " + std::string(g.profile->how) + "  -  waiting for a match (set up on Integrations)";
            } else if (g.profile) {
                detail += "  -  " + std::string(g.profile->how) + ". " + g.profile->note;
            } else if (g.support == Support::Unknown) {
                detail += "  -  no lighting so far. LumaBridge doesn't look inside games with anti-cheat, so it "
                          "switches over as soon as the game sends colors";
            } else {
                detail += "  -  doesn't use a lighting SDK LumaBridge understands";
            }
            break;
        }
        if (screenOn && g.support != Support::Active && g.mode != GameMode::Idle && !builtIn) {
            pill = "Screen colors";
            pillColor = kAccent;
        }
        gameRow(g.game.name, g.color, g.support == Support::Active, detail, pill, pillColor);
    }
    for (const auto& s : others) {
        char detail[160];
        snprintf(detail, sizeof detail, "%s  -  %s  -  updated %llus ago", s.sdk.c_str(), ToHex(s.color).c_str(),
                 static_cast<unsigned long long>((now - s.lastChange) / 1000));
        gameRow(s.game, s.color, true, detail, "Dynamic lighting", kGreen);
    }
    EndCard();

    BeginCard("idle");
    CardTitle(f, "When no game is running");
    int idle = static_cast<int>(ctl.prefs().idle);
    const char* labels[] = {"My manual color", "Rainbow", "Off", "Armoury Crate"};
    if (Segmented("idle", &idle, labels, 4, ImGui::GetContentRegionAvail().x)) {
        ctl.prefs().idle = static_cast<IdleBehavior>(idle);
        ctl.Changed();
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    static const char* kIdleHelp[] = {
        "Your manual color and effect (Lighting > Manual) show between games.",
        "A slow rainbow wave turns around the fans and motherboard between games.",
        "The motherboard and fans stay dark between games.",
        "Between games LumaBridge hands the lights back to Armoury Crate's own effect. Set up "
        "\"Armoury Crate hand-back\" on the Integrations page once to make this silent.",
    };
    Muted("%s", kIdleHelp[idle]);
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (ImGui::Checkbox("Games without dynamic lighting show the screen's colors", &ctl.prefs().screenForUnsupported))
        ctl.Changed();
    Muted("LumaBridge watches the screen image (never the game) and runs its colors around the fans. "
          "Choose per game on the Games List page.");
    EndCard();

    PreviewCard(ctl, f);
    BrightnessCard(ctl, f);
}

void ManualPage(Controller& ctl, UiState& ui, const Fonts& f) {
    Prefs& p = ctl.prefs();
    BeginCard("picker");
    CardTitle(f, "Color");

    const float wheel = std::min(250 * S(), ImGui::GetContentRegionAvail().x * 0.45f);
    float col[3] = {p.manualColor.r / 255.f, p.manualColor.g / 255.f, p.manualColor.b / 255.f};
    ImGui::SetNextItemWidth(wheel);
    if (ImGui::ColorPicker3("##wheel", col,
                            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview |
                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
        p.manualColor = FromV4(col);
        ctl.Changed();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) ctl.RememberManualColor();

    ImGui::SameLine(0, 24 * S());
    ImGui::BeginGroup();
    {
        const float colW = ImGui::GetContentRegionAvail().x;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + colW, pos.y + 56 * S()),
                                                  IM_COL32(p.manualColor.r, p.manualColor.g, p.manualColor.b, 255),
                                                  10 * S());
        ImGui::Dummy(ImVec2(colW, 56 * S()));
        ImGui::Dummy(ImVec2(0, 4 * S()));

        if (!ui.hexEditing) snprintf(ui.hex, sizeof ui.hex, "%s", ToHex(p.manualColor).c_str());
        ImGui::SetNextItemWidth(colW);
        if (ImGui::InputText("##hex", ui.hex, sizeof ui.hex, ImGuiInputTextFlags_CharsUppercase)) {
            Rgb c;
            if (FromHex(ui.hex, &c)) {
                p.manualColor = c;
                ctl.Changed();
            }
        }
        ui.hexEditing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) ctl.RememberManualColor();

        ImGui::Dummy(ImVec2(0, 6 * S()));
        ImGui::TextUnformatted("Effect");
        if (EffectGrid(&p.effect, colW)) {
            const EffectInfo& e = kEffects[static_cast<int>(p.effect)];
            if (p.effect == ManualEffect::ColorCycle) p.speedHz = std::min(p.speedHz, 0.5f);
            else if (e.maxSpeed > 0) p.speedHz = std::clamp(p.speedHz, e.minSpeed, e.maxSpeed);
            ctl.Changed();
        }
        const EffectInfo& e = kEffects[static_cast<int>(p.effect)];
        Muted("%s", e.help);
        if (p.effect == ManualEffect::ColorCycle) {
            float seconds = 1.f / std::max(p.speedHz, 0.02f);
            if (LabeledSlider("One full cycle every", &seconds, 2.f, 60.f, "%.0f seconds")) {
                p.speedHz = 1.f / seconds;
                ctl.Changed();
            }
        } else if (e.maxSpeed > 0) {
            p.speedHz = std::clamp(p.speedHz, e.minSpeed, e.maxSpeed);
            if (LabeledSlider("Speed", &p.speedHz, e.minSpeed, e.maxSpeed, e.speedFmt)) ctl.Changed();
        }
        if (fx::UsesSecondColor(p.effect)) {
            ImGui::Dummy(ImVec2(0, 4 * S()));
            float c2[3] = {p.manualColor2.r / 255.f, p.manualColor2.g / 255.f, p.manualColor2.b / 255.f};
            if (ImGui::ColorEdit3("Second color", c2, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) {
                p.manualColor2 = FromV4(c2);
                ctl.Changed();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Swap")) {
                std::swap(p.manualColor, p.manualColor2);
                ctl.Changed();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Off")) {
                p.manualColor2 = Rgb{};
                ctl.Changed();
            }
        }

        ImGui::Dummy(ImVec2(0, 6 * S()));
        ImGui::TextUnformatted("Presets");
        static const Rgb kPresets[] = {{255, 255, 255}, {255, 40, 40},  {255, 120, 0}, {255, 210, 0},
                                       {60, 230, 90},   {0, 220, 200},  {0, 140, 255}, {80, 70, 255},
                                       {170, 60, 255},  {255, 60, 170}};
        const float sw = 26 * S();
        int perRow = std::max(1, static_cast<int>((colW + 6 * S()) / (sw + 6 * S())));
        for (int i = 0; i < static_cast<int>(std::size(kPresets)); ++i) {
            if (i % perRow) ImGui::SameLine(0, 6 * S());
            char id[8];
            snprintf(id, sizeof id, "p%d", i);
            if (Swatch(id, kPresets[i], sw, p.manualColor == kPresets[i])) {
                p.manualColor = kPresets[i];
                ctl.RememberManualColor();
            }
        }
        if (!p.recentColors.empty()) {
            ImGui::Dummy(ImVec2(0, 4 * S()));
            ImGui::TextUnformatted("Recent");
            for (size_t i = 0; i < p.recentColors.size(); ++i) {
                if (i % perRow) ImGui::SameLine(0, 6 * S());
                char id[8];
                snprintf(id, sizeof id, "r%d", static_cast<int>(i));
                Rgb c = p.recentColors[i];
                if (Swatch(id, c, sw, p.manualColor == c)) {
                    p.manualColor = c;
                    ctl.Changed();
                }
            }
        }
    }
    ImGui::EndGroup();
    EndCard();

    PreviewCard(ctl, f);
    BrightnessCard(ctl, f);
}

void FansCard(Controller& ctl, const Fonts& f) {
    BeginCard("fans");
    CardTitle(f, "Fans on the ARGB header");
    Muted("Per-LED effects (rainbow wave, gradient, comet, twinkle) need to know how the LEDs are "
          "grouped. Fans chained on a hub count in order. be quiet! Light Wings 120 mm: 20 LEDs per fan.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    fx::FanLayout& l = ctl.config().argbFans;
    const float w = std::min(220 * S(), ImGui::GetContentRegionAvail().x / 2 - 8 * S());
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Fans");
    ImGui::SetNextItemWidth(w);
    if (ImGui::SliderInt("##fans", &l.fans, 1, 8, "%d", ImGuiSliderFlags_AlwaysClamp)) ctl.Changed();
    ImGui::EndGroup();
    ImGui::SameLine(0, 16 * S());
    ImGui::BeginGroup();
    ImGui::TextUnformatted("LEDs per fan");
    ImGui::SetNextItemWidth(w);
    if (ImGui::InputInt("##leds", &l.ledsPerFan, 1, 4)) {
        l.ledsPerFan = l.LedsPerFan();
        ctl.Changed();
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 4 * S()));
    int span = l.repeatPerFan ? 0 : 1;
    const char* modes[] = {"Same pattern on every fan", "One pattern across all fans"};
    if (Segmented("fanmode", &span, modes, 2, ImGui::GetContentRegionAvail().x)) {
        l.repeatPerFan = span == 0;
        ctl.Changed();
    }
    bool test = ctl.fanTest();
    if (ImGui::Checkbox("Show test pattern", &test)) ctl.SetFanTest(test);
    ImGui::SameLine();
    Muted("Each fan should be one solid color with a single white LED. If a color spills onto the "
          "next fan, change LEDs per fan.");
    if (ctl.fanTest()) {
        ImGui::Dummy(ImVec2(0, 4 * S()));
        LightsPreview(ctl.output(), l, 0);
    }
    EndCard();
}

void PeripheralsCard(Controller& ctl, const Fonts& f) {
    BeginCard("peripherals");
    CardTitle(f, "Keyboard & mouse");

    // Logitech (G502 X Plus, ...) through G HUB.
    const auto& lg = ctl.logitech();
    using S_ = LogitechOutput::State;
    const S_ st = lg.state();
    const bool on = ctl.prefs().logitechDevices;
    IconItem(Icon::Mouse, 20 * S(), on && st == S_::Active ? Hex(kAccent) : Hex(kMuted));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted("Logitech devices");
    ImGui::PopFont();
    ImGui::SameLine();
    if (!on) Pill("Off", kMuted);
    else if (st == S_::Active) Pill("Following LumaBridge", kGreen);
    else if (st == S_::NoGHub) Pill("G HUB not found", kRed);
    else if (st == S_::Waiting) Pill("Waiting for G HUB", kAmber);
    else Pill("G HUB has them", kMuted);
    Muted("Your Logitech mouse and other Logitech RGB gear show LumaBridge's color, through Logitech's own "
          "LED SDK in G HUB (nothing goes into a game). They show one color: the first LED of the effect.");
    if (on && st == S_::Released && !ctl.logitechNote().empty()) Muted("Right now: %s.", ctl.logitechNote().c_str());
    bool enabled = on;
    if (ImGui::Checkbox("Light Logitech devices", &enabled)) ctl.SetLogitechEnabled(enabled);
    if (ImGui::IsItemHovered() && !lg.dllPath().empty()) ImGui::SetTooltip("%s", Utf8(lg.dllPath()).c_str());
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, 6 * S()));

    // ASUS ROG Azoth, by cable or its Omni receiver.
    const auto& az = ctl.azoth();
    using A_ = AzothOutput::State;
    const A_ as = az.state();
    const bool azOn = ctl.prefs().azothKeyboard;
    IconItem(Icon::Keyboard, 20 * S(), azOn && as == A_::Active ? Hex(kAccent) : Hex(kMuted));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted("ASUS ROG Azoth");
    ImGui::PopFont();
    ImGui::SameLine();
    if (!azOn) Pill("Off", kMuted);
    else if (as == A_::Active) Pill(az.wireless() ? "Following LumaBridge (wireless)" : "Following LumaBridge (wired)", kGreen);
    else if (as == A_::NotFound) Pill("Not connected", kAmber);
    else Pill("Armoury Crate's lighting", kMuted);
    ImGui::SameLine();
    Pill("Experimental", kAccent);
    Muted("The whole keyboard shows LumaBridge's color, by cable or wirelessly through its ROG Omni receiver. LumaBridge "
          "sends the same color command Armoury Crate does, but never its save command, so your saved "
          "Armoury Crate lighting stays in the keyboard. When LumaBridge lets go, the keyboard keeps the last "
          "color until it restarts or Armoury Crate sets it again.");
    bool azEnabled = azOn;
    if (ImGui::Checkbox("Light the ROG Azoth", &azEnabled)) ctl.SetAzothEnabled(azEnabled);
    ImGui::SameLine();
    if (ImGui::SmallButton("Run the device probe")) {
        const std::wstring exe = AppDirectory() + L"\\tools\\device-probe.exe";
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::EndGroup();
    EndCard();
}

void DevicesPage(Controller& ctl, const Fonts& f) {
    const sensors::SystemSnapshot snap = ctl.monitor().Snapshot();  // board name for the labels
    auto st = ctl.auraStatus();
    const auto& lastDevices = ctl.devices();

    BeginCard("status");
    CardTitle(f, "Aura connection");
    if (st.connected) {
        Pill("Connected", kGreen);
        ImGui::SameLine();
        Muted("%d device(s) under LumaBridge control. Switched-off devices stay dark while LumaBridge "
              "controls the lights.", static_cast<int>(st.devices.size()));
    } else if (!st.running) {
        Pill("Not controlling the lights", kMuted);
        ImGui::SameLine();
        Muted("%d device(s) found. LumaBridge takes over when a game or your manual color needs the "
              "lights.", static_cast<int>(lastDevices.size()));
    } else {
        Pill("Aura controller not found", kRed);
        ImGui::SameLine();
        Muted("LumaBridge couldn't find the motherboard's Aura USB controller. Details are in the log.");
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (ImGui::Button("Rescan devices")) ctl.RescanDevices();
    EndCard();

    BeginCard("devices");
    CardTitle(f, "Devices");
    if (lastDevices.empty()) {
        Muted("No Aura devices found. Click Rescan devices; if it stays empty, the log (Settings) says why.");
    } else if (ImGui::BeginTable("devtable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 44 * S());
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 150 * S());
        ImGui::TableSetupColumn("LEDs", ImGuiTableColumnFlags_WidthFixed, 50 * S());
        ImGui::TableHeadersRow();
        auto& disabled = ctl.config().auraDisabledDevices;
        for (size_t i = 0; i < lastDevices.size(); ++i) {
            const auto& d = lastDevices[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextColumn();
            auto it = std::find_if(disabled.begin(), disabled.end(),
                                   [&](const std::wstring& n) { return _wcsicmp(n.c_str(), d.name.c_str()) == 0; });
            bool on = it == disabled.end();
            if (ImGui::Checkbox("##on", &on)) {
                if (on) disabled.erase(it);
                else disabled.push_back(d.name);
                ctl.Changed();
            }
            ImGui::TableNextColumn();
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, d.name.c_str(), -1, name, sizeof name, nullptr, nullptr);
            Icon icon;
            const std::string label = DeviceLabel(d, snap, ctl.config().argbFans, &icon);
            IconItem(icon, 16 * S(), on ? Hex(kAccent) : Hex(kMuted));
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name);
            ImGui::TableNextColumn();
            char type[64];
            WideCharToMultiByte(CP_UTF8, 0, AuraDeviceTypeName(d.type), -1, type, sizeof type, nullptr, nullptr);
            Muted("%s", type);
            ImGui::TableNextColumn();
            Muted("%d", d.lightCount);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    EndCard();

    PeripheralsCard(ctl, f);
    FansCard(ctl, f);
}

// Shown on every page while Armoury Crate is taking the lights back: restarting the
// controller takes a few seconds, and nothing visible happens until it's done.
void HandbackBanner(const Controller& ctl, const Fonts& f) {
    const uint64_t left = ctl.handbackMsLeft();
    if (!left) return;
    BeginCard("handback");
    const int seconds = static_cast<int>((left + 999) / 1000);
    ImGui::PushFont(f.bold);
    ImGui::Text("Handing the lights back to Armoury Crate... %d s", seconds);
    ImGui::PopFont();
    Muted("The lighting controller restarts and loads Armoury Crate's effect. The lights keep their "
          "current color until then.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    const float done = 1.f - static_cast<float>(left) / static_cast<float>(Controller::kHandbackMs);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, V4(kAmber));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, V4(kCardHover));
    ImGui::ProgressBar(done, ImVec2(-1, 6 * S()), "");
    ImGui::PopStyleColor(2);
    EndCard();
}

// ---- Games List ------------------------------------------------------------------------

std::string ToUtf8(const std::wstring& w) { return Utf8(w); }

bool ContainsNoCase(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

// The running game (if any) that is this installed game.
const Controller::GameStatus* RunningAs(const Controller& ctl, const InstalledGame& g) {
    const std::wstring dir = games::Lower(g.dir) + L"\\";
    for (const auto& r : ctl.games()) {
        const std::wstring path = games::Lower(r.game.path);
        if (!g.exePath.empty() ? path == games::Lower(g.exePath) : path.rfind(dir, 0) == 0) return &r;
    }
    return nullptr;
}

void GamesListPage(HWND hwnd, Controller& ctl, UiState& ui, const Fonts& f) {
    const auto& lib = ctl.library();
    const auto& seen = ctl.prefs().lightingGames;

    BeginCard("library-head");
    CardTitle(f, "Games on this PC");
    Muted("Found in Steam, Epic, EA, Ubisoft, GOG, Xbox, Riot, Rockstar and the games Windows knows about. "
          "Missing one? Add it, and LumaBridge will recognise it whenever it runs.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (PrimaryButton("Add a game...")) {
        const std::wstring exe = PickExe(hwnd, L"Choose the game's .exe");
        if (!exe.empty()) ctl.AddManualGame(exe);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(ctl.libraryScanning());
    if (ImGui::Button(ctl.libraryScanning() ? "Scanning..." : "Rescan")) ctl.RescanLibrary();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(120 * S(), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##filter", "Search", ui.gameFilter, sizeof ui.gameFilter);
    EndCard();

    // Status per game: lighting now > has sent lighting before > ships SDK files > nothing seen.
    struct Row {
        const InstalledGame* g;
        const Controller::GameStatus* running;
        int rank;  // for sorting: running first
    };
    std::vector<Row> rows;
    for (const auto& g : lib) {
        if (ui.gameFilter[0] && !ContainsNoCase(ToUtf8(g.name), ui.gameFilter) &&
            !ContainsNoCase(ToUtf8(g.store), ui.gameFilter))
            continue;
        const auto* r = RunningAs(ctl, g);
        rows.push_back({&g, r, r ? 0 : 1});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.rank < b.rank; });

    BeginCard("library");
    if (lib.empty()) {
        Muted(ctl.libraryScanning() ? "Looking for games..." : "No games found. Add one with \"Add a game...\".");
    } else {
        Muted("%d game(s)%s", static_cast<int>(lib.size()), ctl.libraryScanning() ? " - rescanning..." : "");
        ImGui::Dummy(ImVec2(0, 2 * S()));
    }
    if (!rows.empty() &&
        ImGui::BeginTable("games", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 2.6f);
        ImGui::TableSetupColumn("Store", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Dynamic lighting", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("Without game lighting", ImGuiTableColumnFlags_WidthStretch, 1.7f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70 * S());
        ImGui::TableHeadersRow();
        int id = 0;
        std::wstring removeExe;
        for (const Row& row : rows) {
            const InstalledGame& g = *row.g;
            ImGui::PushID(id++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string name = ToUtf8(g.name);
            if (row.running) ImGui::PushFont(f.bold);
            ImGui::TextUnformatted(name.c_str());
            if (row.running) ImGui::PopFont();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ToUtf8(g.exePath.empty() ? g.dir : g.exePath).c_str());
            if (row.running) {
                ImGui::SameLine();
                Pill("Running", kAccent);
            }
            ImGui::TableNextColumn();
            Muted("%s", ToUtf8(g.store).c_str());
            ImGui::TableNextColumn();
            bool hasSeen = false;
            for (const auto& exe : g.exeNames) hasSeen |= std::find(seen.begin(), seen.end(), exe) != seen.end();
            const games::GameProfile* profile = row.running ? row.running->profile : nullptr;
            for (size_t i = 0; !profile && i < g.exeNames.size(); ++i) profile = games::FindProfile(g.exeNames[i], "");
            if (!profile) profile = games::FindProfile("", name);
            using games::ProfileKind;
            auto profileTip = [&] {
                if (profile && ImGui::IsItemHovered()) ImGui::SetTooltip("%s.\n%s", profile->how, profile->note);
            };
            if (row.running && row.running->support == games::Support::Active) {
                Pill("Lighting now", kGreen);
            } else if (profile && profile->kind == ProfileKind::NotAGame) {
                Muted("Not a game");
                profileTip();
            } else if (profile && profile->kind == ProfileKind::BuiltIn) {
                Pill("Built in", kGreen);
                profileTip();
            } else if (hasSeen || (row.running && games::SupportsLighting(row.running->support))) {
                Pill("Supported", kGreen);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("LumaBridge has seen this game send lighting.");
            } else if (profile && profile->kind == ProfileKind::VendorSdk) {
                Pill(profile->how, kAmber);
                profileTip();
            } else if (!g.sdk.empty()) {
                const std::string label = "Likely - " + g.sdk;
                Pill(label.c_str(), kAmber);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The game's folder has %s files. It shows as Supported once it sends lighting.",
                                      g.sdk.c_str());
            } else if (profile && profile->kind == ProfileKind::NoSupport) {
                Muted("No lighting support");
                profileTip();
            } else {
                Muted("Not detected");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("No lighting SDK files in its folder and no lighting seen yet. Some games build "
                                      "the SDK in, so play it once with LumaBridge running to be sure.");
            }
            ImGui::TableNextColumn();
            if (!profile || profile->kind != ProfileKind::NotAGame) {
                const std::string key = games::Normalize(name);
                auto& modes = ctl.prefs().gameModes;
                auto it = modes.find(key);
                int mode = it == modes.end() ? 0 : static_cast<int>(it->second);
                const char* labels[] = {"Default", "Screen colors", "My idle choice"};
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##mode", &mode, labels, 3)) {
                    if (mode == 0) modes.erase(key);
                    else modes[key] = static_cast<GameMode>(mode);
                    ctl.Changed();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("What the lights show while this game runs but isn't sending lighting.\n"
                                      "Default follows \"Games without dynamic lighting show the screen's colors\" "
                                      "(Lighting page).");
            }
            ImGui::TableNextColumn();
            if (g.manual && ImGui::SmallButton("Remove")) removeExe = g.exePath;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (!removeExe.empty()) ctl.RemoveManualGame(removeExe);
    }
    EndCard();
}

// ---- Built-in game feeds (Integrations page) ----------------------------------------

// Writes a feed's config file; if the game's folder needs administrator rights, asks for
// them through Write-GameFile.ps1.
void WriteFeedFile(Integrations& in, UiState& ui, const std::string& id, const std::string& what,
                   const std::wstring& path, const std::string& text, bool remove) {
    const bool ok = remove ? (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
                           : WriteTextFile(path, text);
    const DWORD err = GetLastError();
    ui.feedMessageId = id;
    if (ok) {
        ui.feedMessage = "Done: " + what;
    } else if (err == ERROR_ACCESS_DENIED) {
        ui.feedMessageId.clear();  // the elevated run reports under the card itself
        in.WriteGameFileElevated(id, what, path, text, remove);
    } else {
        ui.feedMessage = "Failed: couldn't write " + Utf8(path) + " (error " + std::to_string(err) + ")";
    }
    ui.feedCheckAt = 0;
}

void FeedCards(Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    const uint64_t now = GetTickCount64();
    if (now >= ui.feedCheckAt) {
        ui.feedCheckAt = now + 2000;
        ui.cs2Dir = ctl.GameDir("cs2");
        ui.rlDir = ctl.GameDir("rocketleague");
        ui.cs2Installed = Cs2ConfigInstalled(ui.cs2Dir);
        ui.rlIniFound = !RocketLeagueStatsText(ui.rlDir, true).empty();
        ui.rlEnabled = RocketLeagueStatsEnabled(ui.rlDir);
        ctl.RefreshFeedSettings();
    }
    const auto& feeds = ctl.feeds();
    auto header = [&](const char* id, const char* key, const char* pill, unsigned pillColor) {
        BeginCard(id);
        const games::GameProfile* p = games::ProfileByKey(key);
        ImGui::PushFont(f.bold);
        ImGui::TextUnformatted(p->title);
        ImGui::PopFont();
        ImGui::SameLine();
        Pill(pill, pillColor);
        Muted("%s. %s", p->how, p->note);
        ImGui::Dummy(ImVec2(0, 2 * S()));
    };
    auto footer = [&](const char* id) {
        const std::string msg = ui.feedMessageId == id ? ui.feedMessage
                                : in.LastId() == id    ? in.LastMessage()
                                                       : std::string();
        if (!msg.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, V4(in.Busy() ? kAmber : msg.rfind("Done", 0) == 0 ? kGreen : kRed));
            ImGui::TextWrapped("%s", msg.c_str());
            ImGui::PopStyleColor();
        }
        EndCard();
    };

    // Counter-Strike 2
    {
        const bool on = ui.cs2Installed;
        header("feed-cs2", "cs2", !feeds.Cs2Listening() ? "Port busy" : on ? "Active" : "Off",
               !feeds.Cs2Listening() ? kRed : on ? kGreen : kMuted);
        if (ui.cs2Dir.empty()) {
            Muted("Counter-Strike 2 wasn't found in your game libraries (Games List > Rescan).");
        } else if (on) {
            Muted("%s", feeds.Cs2Seen() ? "CS2 is sending its game state." : "Set up. Restart CS2 once so it picks it up.");
            ImGui::BeginDisabled(in.Busy());
            if (ImGui::Button("Remove##cs2"))
                WriteFeedFile(in, ui, "cs2", "Counter-Strike 2 feed removed", Cs2ConfigPath(ui.cs2Dir), "", true);
            ImGui::EndDisabled();
        } else {
            ImGui::BeginDisabled(in.Busy());
            if (PrimaryButton("Set up##cs2"))
                WriteFeedFile(in, ui, "cs2", "Counter-Strike 2 feed set up - restart CS2",
                              Cs2ConfigPath(ui.cs2Dir), Cs2ConfigText(), false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            Muted("Adds gamestate_integration_lumabridge.cfg to CS2's cfg folder (Valve's official way).");
        }
        footer("cs2");
    }

    // Rocket League
    {
        const bool on = ui.rlEnabled;
        header("feed-rl", "rocketleague", on ? "Active" : "Off", on ? kGreen : kMuted);
        if (ui.rlDir.empty()) {
            Muted("Rocket League wasn't found in your game libraries (Games List > Rescan).");
        } else if (!ui.rlIniFound) {
            Muted("This Rocket League install has no Stats API settings file (TAGame\\Config\\DefaultStatsAPI.ini). "
                  "Update the game; the Stats API came in a 2025 update.");
        } else {
            if (on)
                Muted("%s", feeds.RocketLeagueConnected() ? "Connected to the game."
                                                          : "Switched on. Restart Rocket League if it's running.");
            ImGui::BeginDisabled(in.Busy());
            if (on ? ImGui::Button("Switch off##rl") : PrimaryButton("Switch on##rl"))
                WriteFeedFile(in, ui, "rocketleague",
                              on ? "Rocket League Stats API switched off" : "Rocket League Stats API switched on - restart the game",
                              RocketLeagueStatsIni(ui.rlDir), RocketLeagueStatsText(ui.rlDir, !on), false);
            ImGui::EndDisabled();
            if (!on) {
                ImGui::SameLine();
                Muted("Sets PacketSendRate in the game's DefaultStatsAPI.ini (a backup is kept).");
            }
        }
        footer("rocketleague");
    }

    // War Thunder
    header("feed-wt", "warthunder", "Built in", kGreen);
    Muted("%s", feeds.WarThunderSeen() ? "Seen War Thunder's status page this session."
                                       : "Works as soon as you're in a battle.");
    footer("warthunder");
}

void IntegrationsPage(HWND hwnd, Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    const auto& gs = ctl.gameSense();
    in.SetOwner(hwnd);
    if (!ui.integrationsLoaded || in.TakeFinished()) {
        in.Refresh(gs.IsRunning(), gs.Port(), gs.CorePropsWritten(), gs.FoundSteelSeriesGG(), gs.ForwardPort(),
                   gs.ForwardOk());
        ui.integrationsLoaded = true;
    }

    Muted("LumaBridge answers games as if the vendor's software and devices were installed, then sends "
          "the colors to your Aura devices.");
    ImGui::Dummy(ImVec2(0, 4 * S()));

    for (const auto& it : in.list()) {
        BeginCard(it.id.c_str());
        ImGui::PushFont(f.bold);
        ImGui::TextUnformatted(it.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        switch (it.state) {
        case IntegrationState::Active: Pill("Active", kGreen); break;
        case IntegrationState::NotInstalled: Pill("Off", kMuted); break;
        case IntegrationState::Conflict:
            Pill(it.id == "gamesense" ? "GG not answering" : "Vendor software present", kAmber);
            break;
        case IntegrationState::PerGame: Pill("Per game", kAccent); break;
        case IntegrationState::Problem: Pill(it.id == "handback" ? "Update needed" : "Needs repair", kRed); break;
        }
        Muted("%s", it.description.c_str());
        Muted("%s", it.detail.c_str());
        ImGui::Dummy(ImVec2(0, 2 * S()));

        ImGui::BeginDisabled(in.Busy());
        if (it.id == "gamesense") {
            bool on = gs.IsRunning();
            if (ImGui::Checkbox("Enabled", &on)) {
                ctl.SetGameSenseEnabled(on);
                ui.integrationsLoaded = false;
            }
            if (it.state == IntegrationState::Problem) {
                ImGui::SameLine();
                if (PrimaryButton("Repair")) in.Install("gamesense");
            }
        } else if (it.id == "corsair") {
            if (PrimaryButton("Add to a game...")) {
                std::wstring dir = PickFolder(hwnd, L"Choose the game's folder (where its .exe is)");
                if (!dir.empty()) in.Install("corsair", dir);
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove from a game...")) {
                std::wstring dir = PickFolder(hwnd, L"Choose the game's folder");
                if (!dir.empty()) in.Remove("corsair", dir);
            }
        } else if (it.id == "handback" && it.state != IntegrationState::NotInstalled) {
            // Setting it up again replaces the task (needed after updates that change it).
            if (it.state == IntegrationState::Problem ? PrimaryButton("Update") : ImGui::Button("Set up again"))
                in.Install(it.id);
            ImGui::SameLine();
            if (ImGui::Button("Remove")) in.Remove(it.id);
        } else if (it.state == IntegrationState::Active) {
            if (ImGui::Button("Remove")) in.Remove(it.id);
        } else if (it.state == IntegrationState::Conflict) {
            if (ImGui::Button("Replace vendor runtime")) in.Install(it.id, L"", true);
        } else {
            if (PrimaryButton(it.id == "logitech" || it.id == "handback" ? "Set up" : "Install")) in.Install(it.id);
        }
        ImGui::EndDisabled();
        // The result of the last action, right under its button.
        if (in.LastId() == it.id) {
            const std::string msg = in.LastMessage();
            if (!msg.empty()) {
                ImGui::Dummy(ImVec2(0, 2 * S()));
                const bool bad = msg.rfind("Failed", 0) == 0 || msg.rfind("Could not", 0) == 0 ||
                                 msg.rfind("Cancelled", 0) == 0;
                ImGui::PushStyleColor(ImGuiCol_Text, V4(in.Busy() ? kAmber : bad ? kRed : kGreen));
                ImGui::TextWrapped("%s", msg.c_str());
                ImGui::PopStyleColor();
            }
        }
        EndCard();
    }

    ImGui::Dummy(ImVec2(0, 6 * S()));
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted("Built-in game feeds");
    ImGui::PopFont();
    Muted("Official data these games publish on your PC. Nothing is added to the game itself, so "
          "anti-cheat isn't involved.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    FeedCards(ctl, in, ui, f);
}

void SettingsPage(Controller& ctl, UiState& ui, const Fonts& f) {
    Config& cfg = ctl.config();

    BeginCard("calibration");
    CardTitle(f, "Color calibration");
    Muted("Aura LEDs often look bluer or brighter than other brands. Nudge these until the colors match.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    float r = static_cast<float>(cfg.auraCorrection.gainR * 100), g = static_cast<float>(cfg.auraCorrection.gainG * 100),
          b = static_cast<float>(cfg.auraCorrection.gainB * 100), gamma = static_cast<float>(cfg.auraCorrection.gamma);
    bool changed = false;
    changed |= LabeledSlider("Red", &r, 0, 150, "%.0f%%");
    changed |= LabeledSlider("Green", &g, 0, 150, "%.0f%%");
    changed |= LabeledSlider("Blue", &b, 0, 150, "%.0f%%");
    changed |= LabeledSlider("Gamma", &gamma, 0.5f, 3.f, "%.2f");
    if (changed) {
        cfg.auraCorrection.gainR = r / 100.0;
        cfg.auraCorrection.gainG = g / 100.0;
        cfg.auraCorrection.gainB = b / 100.0;
        cfg.auraCorrection.gamma = gamma;
        ctl.Changed();
    }
    if (ImGui::Button("Reset calibration")) {
        double brightness = cfg.auraCorrection.brightness;
        cfg.auraCorrection = ColorCorrection{};
        cfg.auraCorrection.brightness = brightness;
        ctl.Changed();
    }
    EndCard();

    BeginCard("dynamic");
    CardTitle(f, "Dynamic lighting");
    int mode = cfg.bitmapReduce == BitmapReduce::Brightest ? 1 : 0;
    ImGui::TextUnformatted("Per-key effects become one color by");
    const char* modes[] = {"Average of lit keys", "Brightest key"};
    if (Segmented("reduce", &mode, modes, 2, ImGui::GetContentRegionAvail().x)) {
        cfg.bitmapReduce = mode ? BitmapReduce::Brightest : BitmapReduce::Average;
        ctl.Changed();
    }
    Muted("Takes effect the next time a game starts its lighting.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    float hz = static_cast<float>(cfg.maxUpdateHz);
    if (LabeledSlider("Maximum update rate", &hz, 10, 60, "%.0f per second")) {
        cfg.maxUpdateHz = static_cast<int>(hz);
        ctl.Changed();
    }
    EndCard();

    BeginCard("startup");
    CardTitle(f, "Startup");
    if (!ui.autostartLoaded) {
        ui.autostart = IsAutostartEnabled();
        ui.autostartLoaded = true;
    }
    if (ImGui::Checkbox("Start LumaBridge with Windows", &ui.autostart)) SetAutostart(ui.autostart);
    if (ImGui::Checkbox("Start minimized to the tray", &ctl.prefs().startMinimized)) ctl.Changed();
    EndCard();

    BeginCard("handback");
    CardTitle(f, "Armoury Crate");
    Muted("Stopping hands the motherboard and fans back to Armoury Crate's own effect. Exiting "
          "LumaBridge does the same. With \"Armoury Crate hand-back\" set up (Integrations page) this is "
          "silent; otherwise Armoury Crate opens and you click an effect once.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (ctl.auraPaused()) {
        if (PrimaryButton("Resume lighting control")) ctl.ResumeAura();
    } else if (ImGui::Button("Stop controlling the lights")) {
        ctl.StopLighting();
    }
    EndCard();

    BeginCard("about");
    CardTitle(f, "About");
    ImGui::Text("LumaBridge %s", kVersionText);
    Muted("Game lighting for ASUS Aura, without Armoury Crate in the way.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (ImGui::Button("Releases on GitHub"))
        ShellExecuteW(nullptr, L"open", L"https://github.com/EmilB04/LumaBridge/releases", nullptr, nullptr, SW_SHOWNORMAL);
    EndCard();

    BeginCard("trouble");
    CardTitle(f, "Troubleshooting");
    if (ImGui::Button("Open log folder")) {
        std::wstring dir = ctl.logPath().substr(0, ctl.logPath().find_last_of(L"\\/"));
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open settings file"))
        ShellExecuteW(nullptr, L"open", L"notepad.exe", ctl.configPath().c_str(), nullptr, SW_SHOWNORMAL);
    EndCard();
}

// ---- Frame ---------------------------------------------------------------------------

void Sidebar(Controller& ctl, UiState& ui, const Fonts& f, float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(kSidebar));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * S(), 20 * S()));
    ImGui::BeginChild("sidebar", ImVec2(width, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PushFont(f.title);
    ImGui::TextUnformatted("LumaBridge");
    ImGui::PopFont();
    ImGui::PushFont(f.caption);
    Muted("Game lighting for Aura");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 18 * S()));

    const struct {
        Page page;
        const char* label;
    } items[] = {{Page::Dashboard, "Dashboard"},       {Page::Lighting, "Lighting"},
                 {Page::GamesList, "Games List"},
                 {Page::Devices, "Devices"},           {Page::Integrations, "Integrations"},
                 {Page::Settings, "Settings"}};
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8 * S());
    for (const auto& it : items) {
        const bool sel = ui.page == it.page;
        ImGui::PushStyleColor(ImGuiCol_Header, V4(kAccent, 0.22f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, V4(kCardHover));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, V4(kAccent, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? V4(kText) : V4(kMuted));
        if (sel) ImGui::PushFont(f.bold);
        if (ImGui::Selectable(it.label, sel, 0, ImVec2(0, 36 * S()))) ui.page = it.page;
        if (sel) ImGui::PopFont();
        ImGui::PopStyleColor(4);
    }
    ImGui::PopStyleVar(2);

    // Live output at the bottom of the sidebar.
    const auto& out = ctl.output();
    const float orbR = 22 * S();
    const float bottom = ImGui::GetWindowHeight() - 20 * S();
    ImGui::SetCursorPosY(bottom - orbR * 2 - 76 * S());  // leaves room for the version line
    ImVec2 p = ImGui::GetCursorScreenPos();
    Orb(ImVec2(p.x + orbR + 4 * S(), p.y + orbR + 4 * S()), orbR, PreviewColor(out));
    ImGui::Dummy(ImVec2(0, orbR * 2 + 14 * S()));
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted(ctl.prefs().mode == Mode::Auto ? "Auto" : "Manual");
    ImGui::PopFont();
    // Version in the bottom-left corner.
    {
        ImGui::PushFont(f.caption);
        const ImVec2 size = ImGui::CalcTextSize(kVersionText);
        const ImVec2 at(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x,
                        ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - size.y - 8 * S());
        ImGui::GetWindowDrawList()->AddText(at, Hex(kMuted, 160), kVersionText);
        ImGui::PopFont();
    }
    ImGui::PushFont(f.caption);
    if (const uint64_t left = ctl.handbackMsLeft())
        Muted("Handing back to Armoury Crate... %d s", static_cast<int>((left + 999) / 1000));
    else
        Muted("%s", out.label.c_str());
    ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

}  // namespace

void ApplyTheme(float scale) {
    g_scale = scale;
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 10);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.FrameRounding = 8;
    s.GrabRounding = 8;
    s.GrabMinSize = 14;
    s.ChildRounding = 12;
    s.PopupRounding = 10;
    s.ScrollbarRounding = 8;
    s.ScrollbarSize = 12;
    s.CellPadding = ImVec2(8, 8);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = V4(kText);
    c[ImGuiCol_TextDisabled] = V4(kMuted);
    c[ImGuiCol_WindowBg] = V4(kBg);
    c[ImGuiCol_ChildBg] = V4(kBg);
    c[ImGuiCol_PopupBg] = V4(kCard);
    c[ImGuiCol_Border] = V4(kBorder);
    c[ImGuiCol_FrameBg] = V4(kCardHover);
    c[ImGuiCol_FrameBgHovered] = V4(kBorder);
    c[ImGuiCol_FrameBgActive] = V4(kBorder);
    c[ImGuiCol_Button] = V4(kCardHover);
    c[ImGuiCol_ButtonHovered] = V4(kBorder);
    c[ImGuiCol_ButtonActive] = V4(kAccent, 0.6f);
    c[ImGuiCol_SliderGrab] = V4(kAccent);
    c[ImGuiCol_SliderGrabActive] = V4(kAccentHover);
    c[ImGuiCol_CheckMark] = V4(kAccentHover);
    c[ImGuiCol_Header] = V4(kAccent, 0.25f);
    c[ImGuiCol_HeaderHovered] = V4(kCardHover);
    c[ImGuiCol_HeaderActive] = V4(kAccent, 0.35f);
    c[ImGuiCol_TableHeaderBg] = V4(kCard);
    c[ImGuiCol_TableRowBg] = V4(kCard);
    c[ImGuiCol_TableRowBgAlt] = V4(kCardHover, 0.5f);
    c[ImGuiCol_TableBorderLight] = V4(kBorder);
    c[ImGuiCol_ScrollbarBg] = V4(kBg, 0.f);
    c[ImGuiCol_ScrollbarGrab] = V4(kBorder);
    c[ImGuiCol_TextSelectedBg] = V4(kAccent, 0.4f);
    s.ScaleAllSizes(scale);
}

void DrawUi(HWND hwnd, Controller& ctl, Integrations& integrations, UiState& ui, const Fonts& f) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("LumaBridge", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float sidebarW = 210 * S();
    Sidebar(ctl, ui, f, sidebarW);
    ImGui::SameLine(0, 0);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * S(), 22 * S()));
    ImGui::BeginChild("main", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);

    // Header: page title + global mode switch.
    const char* titles[] = {"Dashboard", "Lighting", "Games List", "Devices", "Integrations", "Settings"};  // Page order
    ImGui::PushFont(f.title);
    ImGui::TextUnformatted(titles[static_cast<int>(ui.page)]);
    ImGui::PopFont();
    {
        int mode = ctl.prefs().mode == Mode::Manual ? 1 : 0;
        const char* labels[] = {"Auto", "Manual"};
        const float w = 220 * S();
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - w);
        if (Segmented("mode", &mode, labels, 2, w)) {
            ctl.prefs().mode = mode ? Mode::Manual : Mode::Auto;
            ctl.Changed();
        }
    }
    ImGui::Dummy(ImVec2(0, 8 * S()));

    HandbackBanner(ctl, f);

    if (ctl.auraPaused()) {
        BeginCard("paused");
        const bool byUser = ctl.pause() == Controller::Pause::ByUser;
        Pill(byUser ? "Not controlling the lights" : "Lighting control paused", kAmber);
        ImGui::Dummy(ImVec2(0, 2 * S()));
        if (byUser && ctl.handbackMsLeft() > 0)
            Muted("Handing back to Armoury Crate - see the countdown above.");
        else if (byUser)
            Muted("You stopped LumaBridge's lighting and handed it back to Armoury Crate.");
        else
            Muted("LumaBridge closed unexpectedly the last time it controlled your lights, so it is "
                  "leaving them alone for now. Details are in the log folder (Settings).");
        if (PrimaryButton("Resume lighting control")) ctl.ResumeAura();
        EndCard();
    }

    // Re-check integration status whenever the Integrations page is opened.
    if (ui.page == Page::Integrations && ui.lastPage != Page::Integrations) ui.integrationsLoaded = false;
    if (ui.page != Page::Devices && ctl.fanTest()) ctl.SetFanTest(false);  // the test is a Devices-page thing
    ui.lastPage = ui.page;

    switch (ui.page) {
    case Page::Lighting:
        if (ctl.prefs().mode == Mode::Auto) AutoPage(ctl, f);
        else ManualPage(ctl, ui, f);
        break;
    case Page::Devices: DevicesPage(ctl, f); break;
    case Page::Dashboard: DashboardPage(hwnd, ctl, integrations, ui, f); break;
    case Page::GamesList: GamesListPage(hwnd, ctl, ui, f); break;
    case Page::Integrations: IntegrationsPage(hwnd, ctl, integrations, ui, f); break;
    case Page::Settings: SettingsPage(ctl, ui, f); break;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

}  // namespace luma::app
