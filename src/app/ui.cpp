#include "ui.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <functional>
#include <map>
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
#include "imgui_internal.h"
#include "integrations.h"
#include "setup_hardware.h"
#include "vendor_detect.h"
#include "azoth_layout.h"
#include "logitech_hidpp.h"
#include "openrgb_protocol.h"
#include "lamparray.h"
#include "device_catalog.h"

namespace luma::app {
namespace {

// ---- Palette -------------------------------------------------------------------------

constexpr ImU32 Hex(unsigned rgb, unsigned a = 255) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}
ImVec4 V4(unsigned rgb, float a = 1.f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}

constexpr unsigned kBg = 0x0B0D12;
constexpr unsigned kSidebar = 0x0F1218;
constexpr unsigned kCard = 0x151922;
constexpr unsigned kCardHover = 0x1D2230;
constexpr unsigned kTrack = 0x0F1219;  // behind segmented controls and switches
constexpr unsigned kBorder = 0x252B3A;
constexpr unsigned kText = 0xECEEF4;
constexpr unsigned kMuted = 0x8A93A8;
constexpr unsigned kAccent = 0x7C6CFF;
constexpr unsigned kAccentHover = 0x9384FF;
constexpr unsigned kAccent2 = 0x3CC8FF;  // the other end of accent gradients
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

bool BeginCard(const char* id, float height = 0, ImGuiWindowFlags windowFlags = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(kCard));
    ImGui::PushStyleColor(ImGuiCol_Border, V4(kBorder, 0.7f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * S());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * S(), 18 * S()));
    ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (height <= 0) flags |= ImGuiChildFlags_AutoResizeY;
    bool open = ImGui::BeginChild(id, ImVec2(0, height), flags, windowFlags);
    return open;
}

void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, 6 * S()));
}

void CardTitle(const Fonts& f, const char* title) {
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2 * S()));
}

// Label text of an id ("Name##id" -> "Name").
const char* LabelEnd(const char* label) { return ImGui::FindRenderedTextEnd(label); }

// Segmented control: a rounded track with the selected option as an accent pill. Returns
// true when the selection changed.
bool Segmented(const char* id, int* current, const char* const* labels, int count, float width = 0) {
    bool changed = false;
    ImGui::PushID(id);
    const float pad = 3 * S();
    const float h = ImGui::GetFrameHeight() + 4 * S();
    const float bh = h - pad * 2;
    float total = width;
    if (total <= 0) {
        total = pad * 2;
        for (int i = 0; i < count; ++i) total += ImGui::CalcTextSize(labels[i], nullptr, true).x + 32 * S();
    }
    const float bw = (total - pad * 2) / count;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + total, p.y + h), Hex(kTrack), h / 2);
    dl->AddRect(p, ImVec2(p.x + total, p.y + h), Hex(kBorder, 160), h / 2);
    for (int i = 0; i < count; ++i) {
        const ImVec2 a(p.x + pad + i * bw, p.y + pad);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("seg", ImVec2(bw, bh));
        ImGui::PopID();
        const bool sel = *current == i, hovered = ImGui::IsItemHovered();
        if (sel) {
            dl->AddRectFilled(a, ImVec2(a.x + bw, a.y + bh), Hex(hovered ? kAccentHover : kAccent), bh / 2);
        } else if (hovered) {
            dl->AddRectFilled(a, ImVec2(a.x + bw, a.y + bh), Hex(kCardHover), bh / 2);
        }
        const ImVec2 ts = ImGui::CalcTextSize(labels[i], nullptr, true);
        dl->AddText(ImVec2(a.x + (bw - ts.x) / 2, a.y + (bh - ts.y) / 2), sel ? Hex(0xFFFFFF) : hovered ? Hex(kText) : Hex(kMuted),
                    labels[i], LabelEnd(labels[i]));
        if (clicked && !sel) {
            *current = i;
            changed = true;
        }
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(total, h));
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 2 * S()));  // breathing room before whatever follows
    return changed;
}

// An on/off switch with its label on the right; returns true when flipped. Sits on the
// text baseline like a checkbox, so text after it on the same line lines up.
bool Toggle(const char* label, bool* v) {
    const float fh = ImGui::GetFrameHeight();
    const float h = 20 * S(), w = 36 * S();
    const char* end = LabelEnd(label);
    const float tw = end > label ? ImGui::CalcTextSize(label, end).x + 10 * S() : 0;
    ImGui::AlignTextToFramePadding();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(w + tw, fh));
    if (clicked) *v = !*v;
    // Knob position eases towards the new state.
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetItemID();
    float t = st->GetFloat(id, *v ? 1.f : 0.f);
    t += ((*v ? 1.f : 0.f) - t) * std::min(1.f, ImGui::GetIO().DeltaTime * 16.f);
    st->SetFloat(id, t);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, p.y + (fh - h) / 2), b(p.x + w, p.y + (fh + h) / 2);
    const ImVec4 off = V4(ImGui::IsItemHovered() ? 0x343B4E : kBorder), on = V4(kAccent);
    const ImVec4 mix(off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t, off.z + (on.z - off.z) * t, 1.f);
    dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(mix), h / 2);
    const float r = h / 2 - 3 * S();
    dl->AddCircleFilled(ImVec2(a.x + h / 2 + (w - h) * t, a.y + h / 2), r, Hex(0xFFFFFF), 24);
    if (tw > 0) {
        const float th = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(p.x + w + 10 * S(), p.y + (fh - th) / 2), Hex(kText), label, end);
    }
    return clicked;
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
    if (c.r + c.g + c.b < 90) dl->AddCircle(center, r - 2 * S(), Hex(kBorder), 32, 1.5f * S());  // dark: keep it visible
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
    {"Beat", "Flashes that speed up (a game's bomb).", 0, 0, ""},  // games only: not in the picker
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


// ---- Icons (drawn with lines and shapes, so no icon font is needed) ------------------

enum class Icon {
    Lighting, Game, Cpu, Gpu, Memory, Fan, Temp, Board, Leds, Plug, Info, Mouse, Keyboard, Grid, Gear, Palette,
    Disk, Bolt, Gauge, Sun
};

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
    case Icon::Grid:
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) {
                const float x0 = -r * 0.85f + x * r * 0.95f, y0 = -r * 0.85f + y * r * 0.95f;
                dl->AddRect(At(c, x0, y0), At(c, x0 + r * 0.75f, y0 + r * 0.75f), col, r * 0.18f, 0, t);
            }
        break;
    case Icon::Gear:
        dl->AddCircle(c, r * 0.32f, col, 16, t);
        dl->AddCircle(c, r * 0.62f, col, 24, t);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.785398f;
            dl->AddLine(At(c, std::cos(a) * r * 0.62f, std::sin(a) * r * 0.62f),
                        At(c, std::cos(a) * r * 0.92f, std::sin(a) * r * 0.92f), col, t * 1.6f);
        }
        break;
    case Icon::Palette:
        dl->AddCircle(c, r * 0.85f, col, 28, t);
        for (int i = 0; i < 4; ++i) {
            const float a = -2.4f + i * 0.9f;
            dl->AddCircleFilled(At(c, std::cos(a) * r * 0.48f, std::sin(a) * r * 0.48f), r * 0.14f, col);
        }
        break;
    case Icon::Disk:
        dl->AddRect(At(c, -r * 0.9f, -r * 0.55f), At(c, r * 0.9f, r * 0.55f), col, r * 0.14f, 0, t);
        dl->AddLine(At(c, -r * 0.9f, r * 0.12f), At(c, r * 0.9f, r * 0.12f), col, t);
        dl->AddCircleFilled(At(c, r * 0.55f, r * 0.34f), r * 0.09f, col);
        dl->AddLine(At(c, -r * 0.6f, r * 0.34f), At(c, r * 0.1f, r * 0.34f), col, t);
        break;
    case Icon::Bolt: {
        const ImVec2 pts[] = {At(c, r * 0.15f, -r * 0.95f), At(c, -r * 0.55f, r * 0.12f), At(c, -r * 0.02f, r * 0.12f),
                              At(c, -r * 0.18f, r * 0.95f), At(c, r * 0.55f, -r * 0.18f), At(c, r * 0.02f, -r * 0.18f)};
        dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
        break;
    }
    case Icon::Gauge: {
        dl->PathArcTo(At(c, 0, r * 0.25f), r * 0.85f, 3.14159f, 6.28318f, 24);
        dl->PathStroke(col, 0, t);
        for (int i = 0; i <= 4; ++i) {
            const float a = 3.14159f + i * 0.785398f;
            dl->AddLine(At(c, std::cos(a) * r * 0.62f, r * 0.25f + std::sin(a) * r * 0.62f),
                        At(c, std::cos(a) * r * 0.78f, r * 0.25f + std::sin(a) * r * 0.78f), col, t);
        }
        dl->AddLine(At(c, 0, r * 0.25f), At(c, r * 0.38f, -r * 0.2f), col, t * 1.4f);
        dl->AddCircleFilled(At(c, 0, r * 0.25f), r * 0.13f, col);
        break;
    }
    case Icon::Sun:
        dl->AddCircle(c, r * 0.36f, col, 20, t);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.785398f;
            dl->AddLine(At(c, std::cos(a) * r * 0.58f, std::sin(a) * r * 0.58f),
                        At(c, std::cos(a) * r * 0.88f, std::sin(a) * r * 0.88f), col, t);
        }
        break;
    case Icon::Info:
        dl->AddCircle(c, r * 0.88f, col, 24, t);
        dl->AddCircleFilled(At(c, 0, -r * 0.4f), r * 0.1f, col);
        dl->AddLine(At(c, 0, -r * 0.12f), At(c, 0, r * 0.5f), col, t * 1.2f);
        break;
    }
}

// An icon inline with text (advances the cursor like an item). Centered on the text line,
// including when the line is pushed down to line up with a frame (a switch or button earlier
// in the line or table row), so it never sits higher than the text next to it.
void IconItem(Icon icon, float size, ImU32 col, float spin = 0) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float base = ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;
    const float lh = ImGui::GetTextLineHeight();
    DrawIcon(icon, ImVec2(p.x + size / 2, p.y + base + lh / 2), size, col, spin);
    ImGui::Dummy(ImVec2(size, base + std::max(lh, size * 0.9f)));
}

// Card title with an icon in front.
void CardTitle(const Fonts& f, const char* title, Icon icon) {
    IconItem(icon, 18 * S(), Hex(kAccent));
    ImGui::SameLine(0, 10 * S());
    CardTitle(f, title);
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
    Muted("Fan speeds and CPU / board temperatures: set up Hardware access on the Devices page (one click).");
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
        Muted("Temperature: set up Hardware access (Devices).");
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
        if (load < 0 && temp < 0) Muted("Live stats: NVIDIA cards only for now.");
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
    else if (!shown && !idle) Muted("No fans reported.");
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

// ---- Which devices this PC has ------------------------------------------------------
// The pages show only these. Until the first scan has finished, a device that's switched on
// counts as there.

std::vector<LogitechDevice> LogitechRgb(const Controller& ctl) { return ctl.presence().LogitechRgb(); }

bool HasLogitechRgb(Controller& ctl) {
    const auto& p = ctl.presence();
    return p.scanned ? !p.LogitechRgb().empty() : ctl.prefs().logitechDevices;
}

bool HasAzoth(Controller& ctl) {
    const auto& p = ctl.presence();
    return p.scanned ? p.azoth || ctl.azoth().state() == AzothOutput::State::Active : ctl.prefs().azothKeyboard;
}

// RGB memory LumaBridge can light: sticks the helper found, or the SMBIOS scan says Kingston
// FURY / HyperX (confirmed once the helper looks).
bool HasRgbRam(Controller& ctl, const sensors::SystemSnapshot& snap) {
    return ctl.hardware().sticks() > 0 || DetectSetup(snap.smbios).ram != RamStyle::Generic;
}

// "Logitech G502 X PLUS", or "Logitech devices" for several (or before the scan).
std::string LogitechName(const Controller& ctl) {
    const auto rgb = LogitechRgb(ctl);
    if (rgb.size() == 1 && !rgb[0].name.empty()) return "Logitech " + rgb[0].name;
    return "Logitech devices";
}

// "Mouse", "Mouse and keyboard", ... of the Logitech RGB devices found.
std::string LogitechKinds(const Controller& ctl) {
    std::string out;
    std::vector<int> seen;
    for (const auto& d : LogitechRgb(ctl)) {
        if (std::find(seen.begin(), seen.end(), d.type) != seen.end()) continue;
        seen.push_back(d.type);
        std::string k = hidpp::DeviceTypeName(d.type);
        if (!out.empty()) {
            for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            out += " and ";
        }
        out += k;
    }
    return out.empty() ? "Logitech gear" : out;
}

// OpenRGB devices LumaBridge lights (switched on, and not lit natively).
std::vector<OpenRgbDevice> OpenRgbLit(Controller& ctl) {
    std::vector<OpenRgbDevice> out;
    if (!ctl.prefs().openRgb) return out;
    for (const auto& d : ctl.openRgb().devices())
        if (ctl.OpenRgbOn(d)) out.push_back(d);
    return out;
}

// LampArray devices LumaBridge lights (switched on, and not lit another way).
std::vector<LampArrayDevice> LampArrayLit(Controller& ctl) {
    std::vector<LampArrayDevice> out;
    if (!ctl.prefs().lampArray) return out;
    for (const auto& d : ctl.lampArray().devices())
        if (ctl.LampArrayOn(d)) out.push_back(d);
    return out;
}

// RGB hardware found that LumaBridge can't light directly: brands on USB (except what it
// lights itself: ASUS Aura, Logitech, and devices with Windows' lighting standard), RGB
// memory by part number.
std::vector<std::string> AlsoFound(const Controller& ctl, const sensors::SystemSnapshot& snap) {
    std::vector<uint16_t> native{0x046D, 0x0B05};
    for (const auto& d : ctl.lampArray().devices()) native.push_back(d.vid);
    std::vector<std::string> out = catalog::RgbBrands(ctl.presence().usb, native);
    for (const auto& m : snap.smbios.memory) {
        const std::string line = catalog::RgbMemory(m.manufacturer, m.part);
        if (!line.empty() && std::find(out.begin(), out.end(), line + " memory") == out.end()) out.push_back(line + " memory");
    }
    return out;
}

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + v[i];
    return s;
}

void WDevices(DashCtx& c) {
    const auto& devices = c.ctl.devices();
    int listed = 0;
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
        ++listed;
    }
    if (c.ctl.prefs().logitechDevices && HasLogitechRgb(c.ctl)) {
        const bool active = c.ctl.logitech().state() == LogitechOutput::State::Active;
        IconItem(Icon::Mouse, 18 * S(), active ? Hex(kAccent) : Hex(kMuted));
        ImGui::SameLine();
        ImGui::TextUnformatted(LogitechName(c.ctl).c_str());
        ++listed;
        ImGui::SameLine();
        Muted("%s", active ? "via G HUB" : "G HUB has them");
    }
    if (c.ctl.prefs().ramLighting && HasRgbRam(c.ctl, c.snap)) {
        using R = HardwareHelper::RamState;
        const auto st = c.ctl.hardware().ramState();
        IconItem(Icon::Memory, 18 * S(), st == R::Active ? Hex(kAccent) : Hex(kMuted));
        ImGui::SameLine();
        ImGui::TextUnformatted("RAM");
        ++listed;
        ImGui::SameLine();
        Muted("%s", st == R::Active     ? "following LumaBridge"
                    : st == R::Problem  ? "can't light it (Devices)"
                    : st == R::Released ? "Armoury Crate's lighting"
                    : c.ctl.hardware().state() == HardwareHelper::State::NotSetUp ? "needs hardware access (Devices)"
                                                                                  : "starting...");
    }
    if (c.ctl.prefs().azothKeyboard && HasAzoth(c.ctl)) {
        const auto st = c.ctl.azoth().state();
        IconItem(Icon::Keyboard, 18 * S(), st == AzothOutput::State::Active ? Hex(kAccent) : Hex(kMuted));
        ImGui::SameLine();
        ImGui::TextUnformatted("ASUS ROG Azoth");
        ++listed;
        ImGui::SameLine();
        Muted("%s", st == AzothOutput::State::Active     ? (c.ctl.azoth().wireless() ? "wireless" : "wired")
                    : st == AzothOutput::State::NotFound ? "not connected"
                                                         : "Armoury Crate's lighting");
    }
    for (const auto& d : LampArrayLit(c.ctl)) {
        IconItem(Icon::Leds, 18 * S(), c.ctl.output().stopped ? Hex(kMuted) : Hex(kAccent));
        ImGui::SameLine();
        ImGui::TextUnformatted(d.name.c_str());
        ImGui::SameLine();
        Muted("%u lamps", d.lamps);
        ++listed;
    }
    for (const auto& d : OpenRgbLit(c.ctl)) {
        IconItem(Icon::Leds, 18 * S(), c.ctl.output().stopped ? Hex(kMuted) : Hex(kAccent));
        ImGui::SameLine();
        ImGui::TextUnformatted(d.name.c_str());
        ImGui::SameLine();
        Muted("through OpenRGB");
        ++listed;
    }
    if (!listed) Muted("No RGB devices found yet (Devices > Rescan).");
}

void EnsureIntegrations(Controller& ctl, Integrations& in, UiState& ui) {
    const auto& gs = ctl.gameSense();
    const bool finished = in.TakeFinished();
    if (!ui.integrationsLoaded || finished) {
        in.Refresh(gs.IsRunning(), gs.Port(), gs.CorePropsWritten(), gs.FoundSteelSeriesGG(), gs.ForwardPort(),
                   gs.ForwardOk());
        ui.integrationsLoaded = true;
        if (finished) ctl.hardware().Retry();  // e.g. the hardware helper was just set up
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
    {
        const bool on = c.ctl.prefs().openRgb && c.ctl.openRgb().state() == OpenRgbOutput::State::Connected;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(ImGui::GetCursorScreenPos().x + 5 * S(), ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() / 2),
            4 * S(), Hex(on ? kGreen : kMuted));
        ImGui::Dummy(ImVec2(12 * S(), ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextUnformatted("OpenRGB");
    }
    {
        const bool on = c.ctl.prefs().lampArray && !LampArrayLit(c.ctl).empty();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(ImGui::GetCursorScreenPos().x + 5 * S(), ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() / 2),
            4 * S(), Hex(on ? kGreen : kMuted));
        ImGui::Dummy(ImVec2(12 * S(), ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextUnformatted("Windows Dynamic Lighting devices");
    }
}

void WSystem(DashCtx& c) {
    const auto& s = c.snap;
    const auto& b = s.smbios;
    const float col = ImGui::GetCursorPosX() + 14 * S() + ImGui::GetStyle().ItemSpacing.x +
                      ImGui::CalcTextSize("LumaBridge").x + 16 * S();
    auto line = [&](Icon icon, const char* label, const std::string& v) {
        if (v.empty()) return;
        IconItem(icon, 14 * S(), Hex(kMuted));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::SameLine(col);
        ImGui::TextUnformatted(v.c_str());
    };
    line(Icon::Board, "Board", sensors::FriendlyBoard(b.boardMaker, b.boardName));
    line(Icon::Gear, "BIOS", b.biosVersion.empty() ? std::string() : b.biosVersion + (b.biosDate.empty() ? "" : " (" + b.biosDate + ")"));
    line(Icon::Cpu, "CPU", sensors::FriendlyCpu(s.cpuName));
    for (const auto& g : s.gpus) line(Icon::Gpu, "GPU", sensors::FriendlyGpu(g.name));
    if (s.memTotal) line(Icon::Memory, "Memory", Gb(s.memTotal));
    line(Icon::Lighting, "LumaBridge", kVersionText);
}

// ---- More cards ----------------------------------------------------------------------

// Fixed drives (letter, name, free / total), read again every 15 s.
struct DriveInfo {
    std::string root, label;
    uint64_t free = 0, total = 0;
};

const std::vector<DriveInfo>& Drives() {
    static std::vector<DriveInfo> drives;
    static uint64_t readAt = 0;
    const uint64_t now = GetTickCount64();
    if (readAt && now - readAt < 15000) return drives;
    readAt = now;
    drives.clear();
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER freeToMe{}, total{}, freeAll{};
        if (!GetDiskFreeSpaceExW(root, &freeToMe, &total, &freeAll) || !total.QuadPart) continue;
        wchar_t name[MAX_PATH + 1] = {};
        GetVolumeInformationW(root, name, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        drives.push_back({std::string(1, static_cast<char>('A' + i)) + ":", Utf8(name), freeAll.QuadPart, total.QuadPart});
    }
    return drives;
}

std::string Size(uint64_t bytes) {
    char b[32];
    const double gb = bytes / 1073741824.0;
    if (gb >= 1000) snprintf(b, sizeof b, "%.1f TB", gb / 1024);
    else snprintf(b, sizeof b, "%.0f GB", gb);
    return b;
}

// Text right-aligned on the current line.
void RightText(const char* text, unsigned color = kMuted) {
    const float w = ImGui::CalcTextSize(text).x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
    ImGui::PushStyleColor(ImGuiCol_Text, V4(color));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void WStorage(DashCtx& c) {
    const auto& drives = Drives();
    if (drives.empty()) Muted("No drives found.");
    for (const auto& d : drives) {
        ImGui::PushID(d.root.c_str());
        const double used = 1.0 - static_cast<double>(d.free) / static_cast<double>(d.total);
        IconItem(Icon::Disk, 16 * S(), Hex(kAccent));
        ImGui::SameLine();
        ImGui::PushFont(c.f.bold);
        ImGui::TextUnformatted(d.root.c_str());
        ImGui::PopFont();
        if (!d.label.empty()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));
            ImGui::TextUnformatted(d.label.c_str());
            ImGui::PopStyleColor();
        }
        RightText((Size(d.free) + " free of " + Size(d.total)).c_str());
        Bar(used, used < 0.85 ? kAccent : used < 0.95 ? kAmber : kRed);
        ImGui::PopID();
    }
    int shown = 0;
    for (const auto& x : c.snap.lhm)
        if (x.kind == sensors::HardwareKind::Storage && x.type == sensors::SensorType::Temperature && x.value > 0 &&
            x.value < 120 && shown < 4) {
            if (!shown++) ImGui::Dummy(ImVec2(0, 2 * S()));
            IconItem(Icon::Temp, 14 * S(), Hex(TempColor(x.value)));
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, V4(TempColor(x.value)));
            ImGui::Text("%.0f \xC2\xB0" "C", x.value);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            Muted("%s", x.hardware.empty() ? x.name.c_str() : x.hardware.c_str());
        }
}

void WPower(DashCtx& c) {
    const auto& s = c.snap;
    using sensors::HardwareKind;
    using sensors::SensorType;
    double total = 0;
    int parts = 0;
    auto row = [&](Icon icon, const std::string& label, const char* value) {
        IconItem(icon, 16 * S(), Hex(kAccent));
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());
        RightText(value, kText);
    };
    char b[48];
    const auto* cpuW = sensors::PickSensor(s.lhm, HardwareKind::Cpu, SensorType::Power, {"Package", "CPU Package", "Core"});
    std::vector<std::pair<std::string, double>> gpuW;
    for (const auto& g : s.gpus) {
        double w = g.powerW;
        if (w < 0)
            if (const auto* x = sensors::PickSensor(s.lhm, HardwareKind::Gpu, SensorType::Power, {"Package", "Board", "Core"}))
                w = x->value;
        if (w >= 0) gpuW.push_back({sensors::FriendlyGpu(g.name), w});
    }
    if (cpuW) total += cpuW->value, ++parts;
    for (const auto& g : gpuW) total += g.second, ++parts;
    if (parts) {
        snprintf(b, sizeof b, "%.0f", total);
        Metric(c.f, b, parts > 1 ? "W  CPU + graphics" : "W", kText);
        ImGui::Dummy(ImVec2(0, 2 * S()));
    }
    if (cpuW) {
        snprintf(b, sizeof b, "%.0f W", cpuW->value);
        row(Icon::Cpu, "Processor", b);
    }
    for (const auto& g : gpuW) {
        snprintf(b, sizeof b, "%.0f W", g.second);
        row(Icon::Gpu, g.first, b);
    }
    // Clocks: the fastest core, and the graphics core.
    double cpuClock = -1;
    for (const auto& x : s.lhm)
        if (x.kind == HardwareKind::Cpu && x.type == SensorType::Clock && x.name.find("Core") != std::string::npos)
            cpuClock = std::max(cpuClock, x.value);
    if (cpuClock > 0) {
        snprintf(b, sizeof b, "%.2f GHz", cpuClock / 1000);
        row(Icon::Gauge, "Processor clock", b);
    }
    if (const auto* x = sensors::PickSensor(s.lhm, HardwareKind::Gpu, SensorType::Clock, {"GPU Core", "Core"})) {
        snprintf(b, sizeof b, "%.0f MHz", x->value);
        row(Icon::Gauge, "Graphics clock", b);
    }
    if (!parts && cpuClock <= 0) {
        if (!s.lhmConnected) LhmHint();
        else Muted("No power readings reported.");
    }
}

// ---- The top row: lighting and game ------------------------------------------------

// Card heading: icon, bold title, and optionally a pill on the right.
void CardHeader(const Fonts& f, Icon icon, const char* title, const char* pill = nullptr, unsigned pillColor = kAccent) {
    IconItem(icon, 22 * S(), Hex(kAccent));
    ImGui::SameLine(0, 10 * S());
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (pill) {
        const float w = ImGui::CalcTextSize(pill).x + 20 * S();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
        Pill(pill, pillColor);
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
}

// Pills that wrap onto more lines when they don't fit.
void PillFlow(const std::vector<std::pair<std::string, unsigned>>& pills) {
    const float right = ImGui::GetContentRegionMax().x;
    const float gap = 6 * S();
    for (size_t i = 0; i < pills.size(); ++i) {
        const float w = ImGui::CalcTextSize(pills[i].first.c_str()).x + 20 * S();
        if (i) {
            ImGui::SameLine(0, gap);
            if (ImGui::GetCursorPosX() + w > right) ImGui::NewLine();
        }
        Pill(pills[i].first.c_str(), pills[i].second);
    }
}

// Every device LumaBridge is lighting right now.
int LitDevices(DashCtx& c) {
    if (c.ctl.output().stopped) return 0;
    int n = 0;
    const auto& disabled = c.ctl.config().auraDisabledDevices;
    for (const auto& d : c.ctl.devices()) {
        bool off = false;
        for (const auto& name : disabled) off |= _wcsicmp(name.c_str(), d.name.c_str()) == 0;
        n += !off;
    }
    if (c.ctl.prefs().logitechDevices && c.ctl.logitech().state() == LogitechOutput::State::Active)
        n += static_cast<int>(std::max<size_t>(1, LogitechRgb(c.ctl).size()));
    if (c.ctl.prefs().ramLighting && c.ctl.hardware().ramState() == HardwareHelper::RamState::Active) ++n;
    if (c.ctl.prefs().azothKeyboard && c.ctl.azoth().state() == AzothOutput::State::Active) ++n;
    n += static_cast<int>(LampArrayLit(c.ctl).size() + OpenRgbLit(c.ctl).size());
    return n;
}

void TopLighting(DashCtx& c) {
    const auto& out = c.ctl.output();
    const char* mode = out.stopped ? "Armoury Crate" : c.ctl.prefs().mode == Mode::Auto ? "Auto" : "Manual";
    CardHeader(c.f, Icon::Lighting, "Lighting", mode, out.stopped ? kMuted : kAccent);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float orb = 24 * S();
    Orb(ImVec2(p.x + orb + 4 * S(), p.y + orb + 4 * S()), orb, PreviewColor(out));
    ImGui::Dummy(ImVec2(orb * 2 + 16 * S(), orb * 2 + 8 * S()));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(c.f.bold);
    if (const uint64_t left = c.ctl.handbackMsLeft())
        ImGui::Text("Handing back... %d s", static_cast<int>((left + 999) / 1000));
    else
        ImGui::TextUnformatted(out.label.c_str());
    ImGui::PopFont();
    if (!out.stopped) Muted("%s", kEffects[static_cast<int>(out.fx.kind)].name);
    const int lit = LitDevices(c);
    if (lit) Muted("%d device%s lit", lit, lit == 1 ? "" : "s");
    ImGui::EndGroup();

    // The effect as it runs along a strip of LEDs.
    {
        const int n = 32;
        const float w = ImGui::GetContentRegionAvail().x, h = 10 * S();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float seg = w / n;
        const double t = ImGui::GetTime();
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), Hex(kTrack), h / 2);
        for (int i = 0; i < n; ++i) {
            const Rgb col = out.stopped ? Rgb{60, 64, 76} : fx::Render(out.fx, t, i, n);
            const float x0 = a.x + i * seg, x1 = x0 + seg;
            ImDrawFlags corners = i == 0 ? ImDrawFlags_RoundCornersLeft : i == n - 1 ? ImDrawFlags_RoundCornersRight
                                                                                  : ImDrawFlags_RoundCornersNone;
            dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1 + (i < n - 1 ? 1 : 0), a.y + h), Col(col), h / 2, corners);
        }
        ImGui::Dummy(ImVec2(w, h + 6 * S()));
    }

    IconItem(Icon::Sun, 16 * S(), Hex(kMuted));
    ImGui::SameLine();
    float bright = static_cast<float>(c.ctl.config().auraCorrection.brightness * 100.0);
    const float button = 120 * S();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - 10 * S());
    if (ImGui::SliderFloat("##dash-brightness", &bright, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        c.ctl.config().auraCorrection.brightness = bright / 100.0;
        c.ctl.Changed();
    }
    ImGui::SameLine();
    if (PrimaryButton("Change look", ImVec2(button, 0))) c.ui.page = Page::Lighting;
}

void TopGame(DashCtx& c) {
    const auto& games = c.ctl.games();
    CardHeader(c.f, Icon::Game, "Game", games.empty() ? "Idle" : "Playing", games.empty() ? kMuted : kGreen);
    if (games.empty()) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float sz = 44 * S();
        DrawIcon(Icon::Game, ImVec2(p.x + sz / 2 + 4 * S(), p.y + sz / 2), sz, Hex(kBorder));
        ImGui::Dummy(ImVec2(sz + 16 * S(), sz));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(c.f.bold);
        ImGui::TextUnformatted("No game running");
        ImGui::PopFont();
        Muted("Start a game and its lighting takes over.");
        ImGui::EndGroup();
    }
    for (const auto& g : games) {
        ImGui::PushID(g.game.name.c_str());
        ImGui::PushFont(c.f.bold);
        ImGui::TextUnformatted(g.game.name.c_str());
        ImGui::PopFont();
        const bool active = g.support == games::Support::Active;
        const bool builtIn = g.profile && g.profile->kind == games::ProfileKind::BuiltIn;
        if (active) Pill(("Dynamic lighting - " + g.sdk).c_str(), kGreen);
        else if (c.ctl.screenColorsActive() && g.mode != GameMode::Idle && !builtIn) {
            const std::string problem = c.ctl.screenProblem();
            Pill(problem.empty() ? "Screen colors" : "Screen colors: can't read the screen", problem.empty() ? kAccent : kAmber);
            if (!problem.empty()) Muted("%s", problem.c_str());
        } else if (builtIn || games::SupportsLighting(g.support)) Pill("Waiting for its lighting", kAmber);
        else Pill("No dynamic lighting", kMuted);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    Muted("Built-in game lighting");
    const auto& feeds = c.ctl.feeds();
    const std::pair<const char*, bool> built[] = {{"CS2", feeds.Cs2Seen()},
                                                  {"Rocket League", feeds.RocketLeagueConnected()},
                                                  {"War Thunder", feeds.WarThunderSeen()},
                                                  {"Dota 2", feeds.Dota2Seen()},
                                                  {"League", feeds.LeagueSeen()},
                                                  {"Forza", feeds.ForzaSeen()}};
    std::vector<std::pair<std::string, unsigned>> pills;
    for (const auto& b : built) pills.push_back({b.first, b.second ? kGreen : kMuted});
    PillFlow(pills);
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (ImGui::Button("Games list", ImVec2(120 * S(), 0))) c.ui.page = Page::GamesList;
}

// ---- Layout ---------------------------------------------------------------------------

// The cards below the top row, in sections. Lighting and Game are always on top.
struct DashSection {
    const char* title;
    Icon icon;
};
const DashSection kSections[] = {
    {"Performance", Icon::Gauge}, {"Cooling", Icon::Fan}, {"Devices", Icon::Leds}, {"System", Icon::Info}};

struct DashWidget {
    const char* id;
    const char* title;
    Icon icon;
    int section;
    void (*draw)(DashCtx&);
};

const DashWidget kWidgets[] = {
    {"cpu", "Processor", Icon::Cpu, 0, WCpu},
    {"gpu", "Graphics", Icon::Gpu, 0, WGpu},
    {"memory", "Memory", Icon::Memory, 0, WMemory},
    {"power", "Power and clocks", Icon::Bolt, 0, WPower},
    {"fans", "Fans", Icon::Fan, 1, WFans},
    {"temps", "Temperatures", Icon::Temp, 1, WTemps},
    {"devices", "RGB devices", Icon::Leds, 2, WDevices},
    {"connections", "Connections", Icon::Plug, 2, WConnections},
    {"storage", "Storage", Icon::Disk, 3, WStorage},
    {"system", "System", Icon::Info, 3, WSystem},
};

const DashWidget* FindWidget(const std::string& id) {
    for (const auto& w : kWidgets)
        if (id == w.id) return &w;
    return nullptr;
}

void DashboardCustomize(Controller& ctl, UiState& ui, const Fonts& f) {
    BeginCard("dash-edit");
    CardTitle(f, "Customize the dashboard", Icon::Grid);
    Muted("Lighting and Game always stay on top. Tick the other cards to show them, and use the arrows to change "
          "their order within a section.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    auto& order = ctl.prefs().dashboard;
    bool changed = false;
    for (int sec = 0; sec < static_cast<int>(std::size(kSections)) && !changed; ++sec) {
        ImGui::PushID(sec);
        ImGui::Dummy(ImVec2(0, 2 * S()));
        IconItem(kSections[sec].icon, 16 * S(), Hex(kMuted));
        ImGui::SameLine();
        ImGui::PushFont(f.bold);
        ImGui::TextUnformatted(kSections[sec].title);
        ImGui::PopFont();
        // This section's cards: shown ones in their order, then the hidden ones.
        std::vector<std::string> all;
        for (const auto& id : order)
            if (const DashWidget* w = FindWidget(id); w && w->section == sec) all.push_back(id);
        for (const auto& w : kWidgets)
            if (w.section == sec && std::find(all.begin(), all.end(), w.id) == all.end()) all.push_back(w.id);
        for (size_t i = 0; i < all.size() && !changed; ++i) {
            const DashWidget* w = FindWidget(all[i]);
            ImGui::PushID(w->id);
            ImGui::Indent(20 * S());
            auto pos = std::find(order.begin(), order.end(), all[i]);
            bool shown = pos != order.end();
            if (Toggle("##on", &shown)) {
                if (shown) order.push_back(all[i]);
                else order.erase(pos);
                changed = true;
            }
            ImGui::SameLine();
            IconItem(w->icon, 16 * S(), shown ? Hex(kAccent) : Hex(kMuted));
            ImGui::SameLine();
            ImGui::TextUnformatted(w->title);
            if (!changed && pos != order.end()) {
                // Neighbours in the same section, in the saved order.
                auto prev = order.end(), next = order.end();
                for (auto it = order.begin(); it != order.end(); ++it) {
                    const DashWidget* o = FindWidget(*it);
                    if (!o || o->section != sec || it == pos) continue;
                    if (it < pos) prev = it;
                    else if (next == order.end()) next = it;
                }
                ImGui::SameLine(280 * S());
                ImGui::BeginDisabled(prev == order.end());
                if (ImGui::ArrowButton("up", ImGuiDir_Up)) {
                    std::iter_swap(pos, prev);
                    changed = true;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(next == order.end());
                if (!changed && ImGui::ArrowButton("down", ImGuiDir_Down)) {
                    std::iter_swap(pos, next);
                    changed = true;
                }
                ImGui::EndDisabled();
            }
            ImGui::Unindent(20 * S());
            ImGui::PopID();
        }
        ImGui::PopID();
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

// Content height per card (unscaled), measured last frame: cards in a row are as tall as the
// tallest one, so the grid reads as even tiles instead of a ragged column.
std::map<std::string, float>& Measured() {
    static std::map<std::string, float> measured;
    return measured;
}

// One card in a grid cell, `height` tall (0: its own height).
template <typename Draw>
void GridCard(const char* id, float height, Draw draw) {
    ImGui::PushID(id);
    BeginCard(id, height, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw();
    const ImGuiStyle& st = ImGui::GetStyle();
    Measured()[id] = (ImGui::GetCursorPosY() - st.ItemSpacing.y + st.WindowPadding.y) / S();
    EndCard();
    ImGui::PopID();
}

float RowHeight(const std::vector<std::string>& ids, float min) {
    float h = min;
    for (const auto& id : ids) {
        auto it = Measured().find(id);
        if (it != Measured().end()) h = std::max(h, it->second * S());
    }
    return h;
}

void SectionHeader(const Fonts& f, const DashSection& s) {
    ImGui::Dummy(ImVec2(0, 6 * S()));
    IconItem(s.icon, 18 * S(), Hex(kAccent2));
    ImGui::SameLine(0, 8 * S());
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted(s.title);
    ImGui::PopFont();
    ImGui::SameLine(0, 12 * S());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = p.y + ImGui::GetTextLineHeight() / 2;
    const float right = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, y), ImVec2(right, y), Hex(kBorder), 1.f);
    ImGui::NewLine();
}

void DashboardPage(HWND hwnd, Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    const sensors::SystemSnapshot snap = ctl.monitor().Snapshot();
    DashCtx c{hwnd, ctl, in, ui, f, snap};
    const float avail = ImGui::GetContentRegionAvail().x;

    // Top row: always Lighting and Game.
    {
        const bool side = avail > 620 * S();
        const float h = side ? RowHeight({"top-lighting", "top-game"}, 0) : 0;
        if (ImGui::BeginTable("dash-top", side ? 2 : 1, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            GridCard("top-lighting", h, [&] { TopLighting(c); });
            ImGui::TableNextColumn();
            GridCard("top-game", h, [&] { TopGame(c); });
            ImGui::EndTable();
        }
    }

    if (ImGui::Button(ui.dashEdit ? "Close" : "Customize", ImVec2(110 * S(), 0))) ui.dashEdit = !ui.dashEdit;
    ImGui::SameLine(0, 10 * S());
    ImGui::AlignTextToFramePadding();
    Muted("Hide and reorder the cards below.");
    if (ui.dashEdit) {
        ImGui::Dummy(ImVec2(0, 4 * S()));
        DashboardCustomize(ctl, ui, f);
    }

    bool any = false;
    const int maxCols = avail > 1150 * S() ? 4 : avail > 820 * S() ? 3 : avail > 520 * S() ? 2 : 1;
    for (int sec = 0; sec < static_cast<int>(std::size(kSections)); ++sec) {
        std::vector<const DashWidget*> shown;
        for (const auto& id : ctl.prefs().dashboard)
            if (const DashWidget* w = FindWidget(id); w && w->section == sec) shown.push_back(w);
        if (shown.empty()) continue;
        any = true;
        SectionHeader(f, kSections[sec]);
        // As few rows as fit, spread evenly (4 cards on 3 columns: 2 + 2, not 3 + 1).
        const int n = static_cast<int>(shown.size());
        const int rows = (n + maxCols - 1) / maxCols;
        const int cols = (n + rows - 1) / rows;
        ImGui::PushID(sec);
        if (ImGui::BeginTable("grid", cols, ImGuiTableFlags_SizingStretchSame)) {
            for (int row = 0; row < n; row += cols) {
                std::vector<std::string> ids;
                for (int k = row; k < n && k < row + cols; ++k) ids.push_back(shown[static_cast<size_t>(k)]->id);
                const float rowH = RowHeight(ids, 120 * S());
                ImGui::TableNextRow();
                for (int k = row; k < n && k < row + cols; ++k) {
                    const DashWidget* w = shown[static_cast<size_t>(k)];
                    ImGui::TableNextColumn();
                    GridCard(w->id, rowH, [&] {
                        IconItem(w->icon, 20 * S(), Hex(kAccent));
                        ImGui::SameLine(0, 10 * S());
                        ImGui::PushFont(f.bold);
                        ImGui::TextUnformatted(w->title);
                        ImGui::PopFont();
                        ImGui::Dummy(ImVec2(0, 2 * S()));
                        w->draw(c);
                    });
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
    if (!any) Muted("The other cards are hidden. Click Customize to show some.");
}

// ---- Pages ---------------------------------------------------------------------------

// The overall brightness, or with `device` set, that device's own on top of it.
void BrightnessCard(Controller& ctl, const Fonts& f, const std::string& device = "") {
    BeginCard("brightness");
    if (device.empty()) {
        CardTitle(f, "Brightness", Icon::Lighting);
        float b = static_cast<float>(ctl.config().auraCorrection.brightness * 100.0);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##brightness", &b, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            ctl.config().auraCorrection.brightness = b / 100.0;
            ctl.Changed();
        }
        Muted("Every device, in both Auto and Manual mode. Pick a device above to dim it on its own.");
    } else {
        const std::string title = std::string(device::Name(device)) + " brightness";
        CardTitle(f, title.c_str(), Icon::Lighting);
        float& level = ctl.prefs().deviceLighting[device].brightness;
        float b = level * 100.f;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##devbrightness", &b, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            level = b / 100.f;
            ctl.Changed();
        }
        Muted("Only this device, games included, on top of the overall brightness (%.0f%%, under All devices).",
              ctl.config().auraCorrection.brightness * 100.0);
    }
    EndCard();
}

bool RainbowCard(Controller& ctl, const Fonts& f, Look& p, bool showSpread);

// The main look's rainbow (Auto mode's rainbow between games).
void MainRainbowCard(Controller& ctl, const Fonts& f) {
    Look l = MainLook(ctl.prefs());
    if (RainbowCard(ctl, f, l, true)) {
        SetMainLook(ctl.prefs(), l);
        ctl.Changed();
    }
}

void SetupCard(Controller& ctl, UiState& ui, const Fonts& f, bool selectable);

void AutoPage(Controller& ctl, UiState& ui, const Fonts& f) {
    BeginCard("now");
    CardTitle(f, "Dynamic lighting", Icon::Game);
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
                detail += "  -  " + std::string(g.profile->how) + "  -  waiting for a match (set it up on its Games List page)";
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
    CardTitle(f, "When no game is running", Icon::Lighting);
    int idle = static_cast<int>(ctl.prefs().idle);
    const char* labels[] = {"My manual color", "Rainbow", "Off", "Armoury Crate"};
    if (Segmented("idle", &idle, labels, 4, ImGui::GetContentRegionAvail().x)) {
        ctl.prefs().idle = static_cast<IdleBehavior>(idle);
        ctl.Changed();
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    static const char* kIdleHelp[] = {
        "Your manual color and effect (Lighting > Manual) show between games.",
        "A slow rainbow wave turns around the fans and motherboard between games. Customize it below.",
        "The motherboard and fans stay dark between games.",
        "Between games LumaBridge hands the lights back to Armoury Crate's own effect. Set up "
        "\"Armoury Crate hand-back\" on the Integrations page once to make this silent.",
    };
    Muted("%s", kIdleHelp[idle]);
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (Toggle("Games without dynamic lighting show the screen's colors", &ctl.prefs().screenForUnsupported))
        ctl.Changed();
    Muted("LumaBridge watches the screen image (never the game) and runs its colors around the fans. "
          "Choose per game on the Games List page.");
    EndCard();

    if (ctl.prefs().idle == IdleBehavior::Rainbow) MainRainbowCard(ctl, f);
    SetupCard(ctl, ui, f, false);
    BrightnessCard(ctl, f);
}

// ---- Presets --------------------------------------------------------------------------

// A ready-made look for an effect: colors, speed and (for rainbows) the rainbow settings.
struct Preset {
    const char* name;
    fx::Kind kind;
    Rgb c1, c2;
    float speed;
    float hueStart = 0, hueSpan = 360, saturation = 1;
    int spread = 1;
};

constexpr Rgb H(unsigned rgb) {
    return Rgb{static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)};
}

using K = fx::Kind;
const Preset kPresets[] = {
    {"Arctic", K::Static, H(0x00C8FF), {}, 0},
    {"Sunset", K::Static, H(0xFF6A2B), {}, 0},
    {"Rose", K::Static, H(0xFF3D7F), {}, 0},
    {"Mint", K::Static, H(0x3DFFB0), {}, 0},
    {"Violet", K::Static, H(0x8A5CFF), {}, 0},
    {"Warm white", K::Static, H(0xFFD9A0), {}, 0},
    {"Gold", K::Static, H(0xFFB400), {}, 0},
    {"Crimson", K::Static, H(0xFF1020), {}, 0},

    {"Calm ocean", K::Breathing, H(0x0090FF), {}, 0.25f},
    {"Heartbeat", K::Breathing, H(0xFF1E3C), {}, 1.1f},
    {"Aurora", K::Breathing, H(0x20FFA0), {}, 0.35f},
    {"Lavender", K::Breathing, H(0xB48CFF), {}, 0.3f},
    {"Ember", K::Breathing, H(0xFF5A00), {}, 0.5f},

    {"Alarm", K::Strobe, H(0xFF0010), {}, 3.f},
    {"Flash", K::Strobe, H(0xFFFFFF), {}, 1.5f},
    {"Rave", K::Strobe, H(0xC000FF), {}, 6.f},
    {"Cyan pulse", K::Strobe, H(0x00E5FF), {}, 2.f},

    {"Full spectrum", K::ColorCycle, {}, {}, 0.05f},
    {"Pastel", K::ColorCycle, {}, {}, 0.05f, 0, 360, 0.45f},
    {"Warm", K::ColorCycle, {}, {}, 0.08f, 330, 80},
    {"Ocean", K::ColorCycle, {}, {}, 0.08f, 170, 90},
    {"Quick", K::ColorCycle, {}, {}, 0.2f},

    {"Classic", K::RainbowWave, {}, {}, 0.25f},
    {"Pastel", K::RainbowWave, {}, {}, 0.2f, 0, 360, 0.45f},
    {"Double", K::RainbowWave, {}, {}, 0.2f, 0, 360, 1, 2},
    {"Ocean", K::RainbowWave, {}, {}, 0.3f, 170, 90},
    {"Fire", K::RainbowWave, {}, {}, 0.4f, 0, 45},
    {"Vaporwave", K::RainbowWave, {}, {}, 0.2f, 180, 140},
    {"Toxic", K::RainbowWave, {}, {}, 0.3f, 60, 90},

    {"Sunset", K::Gradient, H(0xFF5E3A), H(0x8A2BE2), 0.1f},
    {"Ocean", K::Gradient, H(0x00C6FF), H(0x0048FF), 0.1f},
    {"Aurora", K::Gradient, H(0x00FFA3), H(0x7B2FF7), 0.15f},
    {"Fire", K::Gradient, H(0xFF1A00), H(0xFFB300), 0.2f},
    {"Cyberpunk", K::Gradient, H(0xFF00C8), H(0x00F0FF), 0.15f},
    {"Peach", K::Gradient, H(0xFF9A8B), H(0xFF3D77), 0.1f},
    {"Forest", K::Gradient, H(0x7CFF4F), H(0x00804A), 0.1f},
    {"Ice", K::Gradient, H(0xFFFFFF), H(0x00A2FF), 0.1f},

    {"Blue comet", K::Comet, H(0x00A0FF), H(0x000814), 0.6f},
    {"Fire trail", K::Comet, H(0xFF6A00), H(0x200000), 0.8f},
    {"Neon", K::Comet, H(0x00FFD0), H(0x14002A), 0.7f},
    {"Ghost", K::Comet, H(0xFFFFFF), H(0x000000), 0.5f},
    {"Red alert", K::Comet, H(0xFF0020), H(0x100000), 1.5f},

    {"Starry night", K::Twinkle, H(0x0A1040), H(0xFFFFFF), 0.5f},
    {"Fireflies", K::Twinkle, H(0x002010), H(0xC8FF3C), 0.4f},
    {"Snowfall", K::Twinkle, H(0x1E3A66), H(0xFFFFFF), 0.6f},
    {"Embers", K::Twinkle, H(0x300600), H(0xFF7A00), 0.7f},
    {"Pink sparkle", K::Twinkle, H(0x2A0020), H(0xFF66D9), 0.6f},
};

bool IsRainbow(fx::Kind k) { return k == fx::Kind::ColorCycle || k == fx::Kind::RainbowWave; }

fx::Params PresetParams(const Preset& p) {
    fx::Params x;
    x.kind = p.kind;
    x.color1 = p.c1;
    x.color2 = p.c2;
    x.speed = p.speed;
    x.hueStart = p.hueStart;
    x.hueSpan = p.hueSpan;
    x.saturation = p.saturation;
    x.spread = p.spread;
    return x;
}

bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

bool PresetActive(const Look& p, const Preset& x) {
    if (p.effect != x.kind || (x.kind != fx::Kind::Static && !Near(p.speedHz, x.speed))) return false;
    if (IsRainbow(x.kind))
        return Near(p.hueStart, x.hueStart) && Near(p.hueSpan, x.hueSpan) && Near(p.saturation, x.saturation) &&
               (x.kind == fx::Kind::ColorCycle || p.spread == x.spread);
    return p.color1 == x.c1 && (!fx::UsesSecondColor(x.kind) || p.color2 == x.c2);
}

void ApplyPreset(Controller& ctl, Look& p, const Preset& x) {
    p.effect = x.kind;
    if (x.kind != fx::Kind::Static) p.speedHz = x.speed;
    if (IsRainbow(x.kind)) {
        p.hueStart = x.hueStart;
        p.hueSpan = x.hueSpan;
        p.saturation = x.saturation;
        if (x.kind == fx::Kind::RainbowWave) p.spread = x.spread;
    } else {
        p.color1 = x.c1;
        if (fx::UsesSecondColor(x.kind)) p.color2 = x.c2;
        ctl.RememberColor(p.color1);
    }
}

// A strip of `fx` as it looks right now, in a bar with rounded ends: the ends are drawn
// as rounded caps in the first / last color, the middle as thin slices over them.
void EffectStrip(const fx::Params& fx, ImVec2 a, ImVec2 b, float rounding) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int n = 32;
    const double t = ImGui::GetTime();
    const float r = std::min(rounding, (b.y - a.y) / 2);
    const float w = (b.x - a.x) / n;
    dl->AddRectFilled(a, ImVec2(a.x + 2 * r, b.y), Col(fx::Render(fx, t, 0, n)), r, ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(b.x - 2 * r, a.y), b, Col(fx::Render(fx, t, n - 1, n)), r, ImDrawFlags_RoundCornersRight);
    for (int i = 0; i < n; ++i) {
        const float x0 = std::max(a.x + i * w, a.x + r), x1 = std::min(a.x + (i + 1) * w + 0.5f, b.x - r);
        if (x1 > x0) dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1, b.y), Col(fx::Render(fx, t, i, n)));
    }
}

// The presets of the current effect as tiles with a live preview; click to apply.
bool PresetTilesOf(Controller& ctl, Look& p, std::initializer_list<fx::Kind> kinds);
bool PresetTiles(Controller& ctl, Look& p) { return PresetTilesOf(ctl, p, {p.effect}); }

// The presets of these effects as tiles; click one to apply it (effect and colors).
bool PresetTilesOf(Controller& ctl, Look& p, std::initializer_list<fx::Kind> kinds) {
    bool changed = false;
    const float w = 128 * S(), h = 60 * S(), gap = 8 * S();
    const float avail = ImGui::GetContentRegionAvail().x;
    const int perRow = std::max(1, static_cast<int>((avail + gap) / (w + gap)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int shown = 0;
    for (size_t i = 0; i < std::size(kPresets); ++i) {
        const Preset& x = kPresets[i];
        if (std::find(kinds.begin(), kinds.end(), x.kind) == kinds.end()) continue;
        if (shown % perRow) ImGui::SameLine(0, gap);
        ++shown;
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("preset", ImVec2(w, h));
        const bool active = PresetActive(p, x), hovered = ImGui::IsItemHovered();
        const ImVec2 b(a.x + w, a.y + h);
        dl->AddRectFilled(a, b, Hex(hovered ? kBorder : kCardHover), 10 * S());
        if (active) dl->AddRect(a, b, Hex(kAccentHover), 10 * S(), 0, 2 * S());
        EffectStrip(PresetParams(x), ImVec2(a.x + 8 * S(), a.y + 8 * S()), ImVec2(b.x - 8 * S(), a.y + 30 * S()), 6 * S());
        dl->AddText(ImVec2(a.x + 10 * S(), a.y + 36 * S()), Hex(active ? kText : hovered ? kText : kMuted), x.name);
        ImGui::PopID();
        if (clicked) {
            ApplyPreset(ctl, p, x);
            changed = true;
        }
    }
    return changed;
}

// ---- Colors ---------------------------------------------------------------------------

// Everything to pick one color: hue wheel, a big preview, hex, preset and recent swatches.
// `slot` keeps the widgets of different colors apart. Returns true when the color changed.
bool ColorEditor(Controller& ctl, UiState& ui, Rgb* color, const char* slot) {
    ImGui::PushID(slot);
    bool changed = false;
    const float wheel = std::min(240 * S(), ImGui::GetContentRegionAvail().x * 0.42f);
    float col[3] = {color->r / 255.f, color->g / 255.f, color->b / 255.f};
    ImGui::SetNextItemWidth(wheel);
    if (ImGui::ColorPicker3("##wheel", col,
                            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview |
                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
        *color = FromV4(col);
        changed = true;
        ctl.Changed();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) ctl.RememberColor(*color);

    ImGui::SameLine(0, 24 * S());
    ImGui::BeginGroup();
    const float colW = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + colW, pos.y + 56 * S()), Col(*color), 12 * S());
    ImGui::GetWindowDrawList()->AddRect(pos, ImVec2(pos.x + colW, pos.y + 56 * S()), Hex(0xFFFFFF, 30), 12 * S());
    ImGui::Dummy(ImVec2(colW, 56 * S()));

    const std::string key = std::string(slot);
    if (!ui.hexEditing || ui.hexSlot != key) snprintf(ui.hex, sizeof ui.hex, "%s", ToHex(*color).c_str());
    ImGui::SetNextItemWidth(colW);
    if (ImGui::InputText("##hex", ui.hex, sizeof ui.hex, ImGuiInputTextFlags_CharsUppercase)) {
        Rgb c;
        if (FromHex(ui.hex, &c)) {
            *color = c;
            changed = true;
            ctl.Changed();
        }
    }
    if (ImGui::IsItemActive()) {
        ui.hexEditing = true;
        ui.hexSlot = key;
    } else if (ui.hexSlot == key) {
        ui.hexEditing = false;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) ctl.RememberColor(*color);

    ImGui::Dummy(ImVec2(0, 2 * S()));
    Muted("Swatches");
    static const Rgb kSwatches[] = {{255, 255, 255}, {255, 40, 40},  {255, 120, 0}, {255, 210, 0},  {60, 230, 90},
                                    {0, 220, 200},   {0, 140, 255},  {80, 70, 255}, {170, 60, 255}, {255, 60, 170},
                                    {255, 170, 110}, {0, 0, 0}};
    const float sw = 26 * S();
    const int perRow = std::max(1, static_cast<int>((colW + 6 * S()) / (sw + 6 * S())));
    for (int i = 0; i < static_cast<int>(std::size(kSwatches)); ++i) {
        if (i % perRow) ImGui::SameLine(0, 6 * S());
        char id[8];
        snprintf(id, sizeof id, "p%d", i);
        if (Swatch(id, kSwatches[i], sw, *color == kSwatches[i])) {
            *color = kSwatches[i];
            changed = true;
            ctl.RememberColor(*color);
        }
        if (kSwatches[i].IsBlack() && ImGui::IsItemHovered()) ImGui::SetTooltip("Off (black)");
    }
    const auto& recent = ctl.prefs().recentColors;
    if (!recent.empty()) {
        ImGui::Dummy(ImVec2(0, 2 * S()));
        Muted("Recent");
        for (size_t i = 0; i < recent.size(); ++i) {
            if (i % perRow) ImGui::SameLine(0, 6 * S());
            char id[8];
            snprintf(id, sizeof id, "r%d", static_cast<int>(i));
            const Rgb c = recent[i];
            if (Swatch(id, c, sw, *color == c)) {
                *color = c;
                changed = true;
                ctl.Changed();
            }
        }
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

// A color as a selectable tab: swatch + name. Returns true when clicked.
bool ColorTab(const char* label, Rgb c, bool selected, float width) {
    const float h = 44 * S();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + width, a.y + h);
    dl->AddRectFilled(a, b, Hex(selected ? kCardHover : hovered ? kCardHover : kTrack), 12 * S());
    dl->AddRect(a, b, selected ? Hex(kAccentHover) : Hex(kBorder), 12 * S(), 0, selected ? 2 * S() : 1 * S());
    const float r = 12 * S();
    const ImVec2 dot(a.x + 14 * S() + r, a.y + h / 2);
    dl->AddCircleFilled(dot, r, Col(c), 24);
    dl->AddCircle(dot, r, Hex(0xFFFFFF, 50), 24, 1.2f * S());
    const float th = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(dot.x + r + 10 * S(), a.y + h / 2 - th), Hex(selected ? kText : kMuted), label, LabelEnd(label));
    const std::string hex = ToHex(c);
    dl->AddText(ImVec2(dot.x + r + 10 * S(), a.y + h / 2), Hex(kMuted), hex.c_str());
    return clicked;
}

// ---- Rainbow --------------------------------------------------------------------------

bool RainbowCard(Controller&, const Fonts& f, Look& p, bool showSpread) {
    BeginCard("rainbow");
    CardTitle(f, "Rainbow", Icon::Palette);
    Muted("Pick which part of the spectrum to use and how vivid it is. Used by Color cycle, Rainbow wave "
          "and Auto mode's rainbow between games.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    // The chosen hues as a strip.
    fx::Params look;
    look.kind = fx::Kind::RainbowWave;
    look.speed = 0;
    look.hueStart = p.hueStart;
    look.hueSpan = p.hueSpan;
    look.saturation = p.saturation;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    EffectStrip(look, a, ImVec2(a.x + w, a.y + 18 * S()), 9 * S());
    ImGui::Dummy(ImVec2(w, 18 * S()));
    bool changed = false;
    changed |= LabeledSlider("Starting hue", &p.hueStart, 0.f, 360.f, "%.0f\xC2\xB0");
    changed |= LabeledSlider("Range of hues", &p.hueSpan, 10.f, 360.f,
                             p.hueSpan >= 359.5f ? "Full spectrum" : "%.0f\xC2\xB0");
    float sat = p.saturation * 100.f;
    if (LabeledSlider("Color intensity", &sat, 0.f, 100.f, sat >= 99.5f ? "Vivid" : "%.0f%%")) {
        p.saturation = sat / 100.f;
        changed = true;
    }
    if (showSpread) {
        ImGui::TextUnformatted("Rainbows around each fan");
        static const char* kSpread[] = {"1", "2", "3", "4"};
        int spread = std::clamp(p.spread, 1, 4) - 1;
        if (Segmented("spread", &spread, kSpread, 4, std::min(320 * S(), ImGui::GetContentRegionAvail().x))) {
            p.spread = spread + 1;
            changed = true;
        }
    }
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (ImGui::Button("Reset rainbow")) {
        p.hueStart = 0;
        p.hueSpan = 360;
        p.saturation = 1;
        p.spread = 1;
        changed = true;
    }
    EndCard();
    return changed;
}

// Everything to edit a look: effect, presets, speed, and its colors or rainbow. Returns
// true when it changed.
bool LookEditor(Controller& ctl, UiState& ui, const Fonts& f, Look& p) {
    bool changed = false;
    BeginCard("effect");
    CardTitle(f, "Effect", Icon::Lighting);
    const float full = ImGui::GetContentRegionAvail().x;
    if (EffectGrid(&p.effect, full)) {
        const EffectInfo& e = kEffects[static_cast<int>(p.effect)];
        if (p.effect == ManualEffect::ColorCycle) p.speedHz = std::min(p.speedHz, 0.5f);
        else if (e.maxSpeed > 0) p.speedHz = std::clamp(p.speedHz, e.minSpeed, e.maxSpeed);
        changed = true;
    }
    const EffectInfo& e = kEffects[static_cast<int>(p.effect)];
    Muted("%s", e.help);
    ImGui::Dummy(ImVec2(0, 4 * S()));
    ImGui::TextUnformatted("Presets");
    changed |= PresetTiles(ctl, p);
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (p.effect == ManualEffect::ColorCycle) {
        float seconds = 1.f / std::max(p.speedHz, 0.02f);
        if (LabeledSlider("One full cycle every", &seconds, 2.f, 60.f, "%.0f seconds")) {
            p.speedHz = 1.f / seconds;
            changed = true;
        }
    } else if (e.maxSpeed > 0) {
        p.speedHz = std::clamp(p.speedHz, e.minSpeed, e.maxSpeed);
        if (LabeledSlider("Speed", &p.speedHz, e.minSpeed, e.maxSpeed, e.speedFmt)) changed = true;
    }
    if (p.effect == ManualEffect::RainbowWave || p.effect == ManualEffect::Gradient || p.effect == ManualEffect::Comet) {
        ImGui::Dummy(ImVec2(0, 2 * S()));
        if (Toggle("Reverse direction", &p.reverse)) changed = true;
    }
    EndCard();

    if (IsRainbow(p.effect)) {
        changed |= RainbowCard(ctl, f, p, p.effect == ManualEffect::RainbowWave);
    } else {
        BeginCard("colors");
        const bool two = fx::UsesSecondColor(p.effect);
        CardTitle(f, two ? "Colors" : "Color", Icon::Palette);
        if (two) {
            static const char* kSecondName[] = {"", "", "", "", "", "Second color", "Background", "Sparkles"};
            const char* first = p.effect == ManualEffect::Twinkle ? "Base color" : p.effect == ManualEffect::Comet ? "Comet" : "First color";
            const float swapW = 70 * S(), gap = 8 * S();
            const float tabW = (ImGui::GetContentRegionAvail().x - swapW - gap * 2) / 2;
            if (ColorTab((std::string(first) + "##c1").c_str(), p.color1, ui.colorSlot == 0, tabW)) ui.colorSlot = 0;
            ImGui::SameLine(0, gap);
            if (ColorTab((std::string(kSecondName[static_cast<int>(p.effect)]) + "##c2").c_str(), p.color2,
                         ui.colorSlot == 1, tabW))
                ui.colorSlot = 1;
            ImGui::SameLine(0, gap);
            if (ImGui::Button("Swap", ImVec2(swapW, 44 * S()))) {
                std::swap(p.color1, p.color2);
                changed = true;
            }
            ImGui::Dummy(ImVec2(0, 6 * S()));
        } else {
            ui.colorSlot = 0;
        }
        if (ui.colorSlot == 0) {
            changed |= ColorEditor(ctl, ui, &p.color1, "color1");
        } else {
            changed |= ColorEditor(ctl, ui, &p.color2, "color2");
        }
        EndCard();
    }

    return changed;
}

// ---- Your setup: every device with its live effect, placed like the real thing -----------

// One thing on the canvas: a fan, the motherboard, the memory, the mouse or the keyboard.
struct SetupItem {
    std::string item;    // key in Prefs::setupSpots
    const char* device;  // device::kFans, ...
    ImVec2 size;
    int fan = -1;        // fans: which one
};

// What a device shows right now (grey while LumaBridge isn't controlling the lights).
const fx::Params* LiveParams(const Controller& ctl, const char* id) {
    return ctl.output().stopped ? nullptr : &ctl.output().For(id);
}

// Also dimmed like the device: the overall brightness times the device's own.
Rgb LiveAt(const fx::Params* p, double t, int i, int n, double level = 1.0) {
    return p ? Scale(fx::Render(*p, t, i, n), level) : Rgb{50, 54, 64};
}

double LiveLevel(Controller& ctl, const char* id) {
    return ctl.config().auraCorrection.brightness * DeviceBrightness(ctl.prefs(), id);
}

void Glow(ImDrawList* dl, ImVec2 c, float r, Rgb col) {
    dl->AddCircleFilled(c, r * 2.4f, Col(col, 38), 20);
    dl->AddCircleFilled(c, r, Col(col), 16);
}

void DrawFan(ImDrawList* dl, ImVec2 a, ImVec2 size, const std::vector<Rgb>& leds, int fan, int per) {
    const ImVec2 c(a.x + size.x / 2, a.y + size.y / 2);
    const float r = size.x / 2;
    dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), Hex(0x0A0C10), 10 * S());
    dl->AddCircleFilled(c, r * 0.9f, Hex(0x07080B), 48);
    dl->AddCircleFilled(c, r * 0.28f, Hex(kCardHover), 32);  // hub
    for (int b = 0; b < 7; ++b) {  // blades
        const float ang = 6.2831853f * b / 7 + static_cast<float>(ImGui::GetTime()) * 0.8f;
        dl->AddLine(ImVec2(c.x + std::cos(ang) * r * 0.3f, c.y + std::sin(ang) * r * 0.3f),
                    ImVec2(c.x + std::cos(ang + 0.5f) * r * 0.72f, c.y + std::sin(ang + 0.5f) * r * 0.72f),
                    Hex(0x2A3040), 3 * S());
    }
    const float ringR = r * 0.8f;
    const float dot = std::clamp(ringR * 3.14159f / per * 0.7f, 1.4f * S(), 4.5f * S());
    for (int i = 0; i < per; ++i) {
        const Rgb led = leds[static_cast<size_t>(fan * per + i)];
        const float ang = -1.5707963f + 6.2831853f * i / per;
        Glow(dl, ImVec2(c.x + std::cos(ang) * ringR, c.y + std::sin(ang) * ringR), dot, led);
    }
}

// Where the memory slots are on the board drawing.
void RamArea(ImVec2 a, ImVec2 size, ImVec2* ra, ImVec2* rb) {
    const float u = size.x / 20;
    *ra = ImVec2(a.x + 12.8f * u, a.y + 1.4f * u);
    *rb = ImVec2(a.x + 18.8f * u, a.y + size.y - 3.2f * u);
}

// One memory stick, seen from above (its light bar): `style` shapes the lit parts; its five
// LEDs run from the top to the bottom.
void DrawStick(ImDrawList* dl, ImVec2 sa, ImVec2 sb, RamStyle style, const fx::Params* p, double t, double level) {
    const float w = sb.x - sa.x, h = sb.y - sa.y;
    dl->AddRectFilled(ImVec2(sa.x - w * 0.12f, sa.y - 2 * S()), ImVec2(sb.x + w * 0.12f, sb.y + 2 * S()), Hex(0x0C0F14),
                      2 * S());  // the stick behind its light bar
    auto ledAt = [&](float frac) {
        const int led = std::clamp(static_cast<int>(frac * 5), 0, 4);
        return LiveAt(p, t, led, 5, level);
    };
    auto part = [&](float y0, float y1, float slant = 0) {
        const Rgb c = ledAt((y0 + y1) / 2);
        const ImVec2 p0(sa.x, sa.y + y0 * h + slant * w), p1(sb.x, sa.y + y0 * h), p2(sb.x, sa.y + y1 * h),
            p3(sa.x, sa.y + y1 * h + slant * w);
        dl->AddRectFilled(ImVec2(sa.x - 3 * S(), sa.y + y0 * h - 1 * S()), ImVec2(sb.x + 3 * S(), sa.y + y1 * h + 1 * S()),
                          Col(c, 28), 3 * S());  // glow
        dl->AddQuadFilled(p0, p1, p2, p3, Col(c));
    };
    switch (style) {
    case RamStyle::KingstonFury: {
        // FURY Beast / Renegade RGB: two blocks, a long bar, the FURY badge, then stripes.
        part(0.00f, 0.07f);
        part(0.09f, 0.16f);
        part(0.18f, 0.56f);
        part(0.58f, 0.73f);
        const float by = sa.y + 0.60f * h;  // the badge's lettering
        dl->AddLine(ImVec2(sa.x + w * 0.35f, by + 0.11f * h), ImVec2(sb.x - w * 0.35f, by + 0.01f * h), Hex(0x000000, 110),
                    std::max(1.f, w * 0.18f));
        for (int i = 0; i < 4; ++i) part(0.76f + i * 0.06f, 0.80f + i * 0.06f);
        break;
    }
    case RamStyle::HyperXFury:
        // FURY RGB: one light bar, cut at an angle.
        for (int i = 0; i < 5; ++i) part(i * 0.2f + 0.01f, i * 0.2f + 0.19f, -0.25f);
        break;
    default:
        for (int i = 0; i < 5; ++i) part(i * 0.2f, i * 0.2f + 0.2f);
        break;
    }
}

// The lit logo on the I/O cover, drawn simply as a filled circle: one slice per LED of the
// board.
void DrawLogoDisc(ImDrawList* dl, ImVec2 c, float r, const std::vector<Rgb>& leds) {
    const int n = std::max(1, static_cast<int>(leds.size()));
    for (int i = 0; i < n; ++i) {
        const Rgb col = leds.empty() ? Rgb{50, 54, 64} : leds[static_cast<size_t>(i)];
        const float a0 = -1.5707963f + 6.2831853f * i / n, a1 = -1.5707963f + 6.2831853f * (i + 1) / n;
        dl->PathLineTo(c);  // glow
        dl->PathArcTo(c, r * 1.35f, a0, a1, 16);
        dl->PathFillConvex(Col(col, 40));
        dl->PathLineTo(c);
        dl->PathArcTo(c, r, a0, a1, 16);
        dl->PathFillConvex(Col(col));
    }
}

// The motherboard as the scan found it: the I/O cover top left (with its lit logo, drawn as
// a filled circle, or the TUF badge on those boards, else its LEDs along the edge), the CPU
// socket, and the memory standing in its slots right of the CPU.
void DrawBoard(ImDrawList* dl, ImVec2 a, ImVec2 size, const std::vector<Rgb>& leds, const SetupHardware& hw,
               const std::array<bool, 4>& slots, bool ramLit, const fx::Params* ram, double t, double ramLevel) {
    const ImVec2 b(a.x + size.x, a.y + size.y);
    auto at = [&](float fx, float fy) { return ImVec2(a.x + fx * size.x, a.y + fy * size.y); };
    dl->AddRectFilled(a, b, Hex(0x0F1218), 6 * S());
    dl->AddRect(a, b, Hex(0x2A3142), 6 * S(), 0, 1.2f * S());
    // The I/O cover.
    const ImVec2 shroud[] = {at(0.02f, 0.02f), at(0.36f, 0.02f), at(0.36f, 0.56f), at(0.26f, 0.72f), at(0.02f, 0.72f)};
    dl->AddConvexPolyFilled(shroud, 5, Hex(0x181C25));
    dl->AddPolyline(shroud, 5, Hex(0x2E3546), ImDrawFlags_Closed, 1 * S());
    for (int i = 0; i < 3; ++i)  // its angled lines, below the logo
        dl->AddLine(at(0.04f, 0.50f - i * 0.06f), at(0.20f - i * 0.05f, 0.70f), Hex(0x232937), 1.5f * S());
    // The CPU socket and a PCIe slot.
    dl->AddRectFilled(at(0.42f, 0.16f), at(0.60f, 0.40f), Hex(0x1C2230), 3 * S());
    dl->AddRect(at(0.42f, 0.16f), at(0.60f, 0.40f), Hex(0x3A4356), 3 * S());
    dl->AddRectFilled(at(0.08f, 0.82f), at(0.60f, 0.86f), Hex(0x222938), 1 * S());

    switch (hw.board) {
    case BoardStyle::Rog:
    case BoardStyle::RogStrix:
        DrawLogoDisc(dl, at(0.19f, 0.34f), size.y * 0.15f, leds);
        break;
    case BoardStyle::Tuf: {
        const Rgb c = leds.empty() ? Rgb{50, 54, 64} : leds.front();
        dl->AddRect(at(0.08f, 0.26f), at(0.30f, 0.42f), Col(c, 60), 3 * S(), 0, 6 * S());
        dl->AddRect(at(0.08f, 0.26f), at(0.30f, 0.42f), Col(c), 3 * S(), 0, 2 * S());
        break;
    }
    default: {
        const int n = static_cast<int>(leds.size());
        for (int i = 0; i < n; ++i) {
            const float y = 0.10f + 0.56f * (n > 1 ? static_cast<float>(i) / (n - 1) : 0.5f);
            Glow(dl, at(0.05f, y), 3 * S(), leds[static_cast<size_t>(i)]);
        }
        break;
    }
    }

    // Four memory slots, A1 nearest the CPU.
    ImVec2 ra, rb;
    RamArea(a, size, &ra, &rb);
    const float pitch = (rb.x - ra.x) / 4, w = pitch * 0.5f;
    for (int slot = 0; slot < 4; ++slot) {
        const float x0 = ra.x + slot * pitch + (pitch - w) / 2;
        const ImVec2 sa(x0, ra.y), sb(x0 + w, rb.y);
        if (!slots[static_cast<size_t>(slot)]) {  // an empty slot
            dl->AddRectFilled(ImVec2(sa.x + w * 0.25f, sa.y), ImVec2(sb.x - w * 0.25f, sb.y), Hex(0x222938), 1 * S());
            continue;
        }
        DrawStick(dl, sa, sb, hw.ram, ramLit ? ram : nullptr, t, ramLevel);
    }
}

// The G502 X Plus from above: its light strip runs along the bottom curve (6 LEDs, thumb
// side first) and up the right side (2).
void DrawMouse(ImDrawList* dl, ImVec2 a, ImVec2 size, const std::vector<Rgb>& leds) {
    const ImVec2 b(a.x + size.x, a.y + size.y);
    const float w = size.x, h = size.y;
    dl->AddRectFilled(ImVec2(a.x + w * 0.08f, a.y), ImVec2(b.x - w * 0.08f, b.y), Hex(0x151922), w * 0.42f);
    dl->AddRect(ImVec2(a.x + w * 0.08f, a.y), ImVec2(b.x - w * 0.08f, b.y), Hex(0x2A3142), w * 0.42f, 0, 1.2f * S());
    const float mid = a.x + w / 2;
    dl->AddLine(ImVec2(mid, a.y + 2 * S()), ImVec2(mid, a.y + h * 0.38f), Hex(0x2A3142), 1.5f * S());       // buttons
    dl->AddRectFilled(ImVec2(mid - w * 0.06f, a.y + h * 0.10f), ImVec2(mid + w * 0.06f, a.y + h * 0.26f), Hex(0x2A3142),
                      w * 0.05f);  // wheel
    dl->AddLine(ImVec2(a.x + w * 0.1f, a.y + h * 0.38f), ImVec2(b.x - w * 0.1f, a.y + h * 0.38f), Hex(0x222838), 1 * S());
    const float cx = a.x + w / 2, cy = a.y + h * 0.66f, rx = w * 0.36f, ry = h * 0.24f;
    for (int i = 0; i < 6; ++i) {  // bottom curve, left to right
        const float ang = 3.14159f * (0.92f - 0.62f * i / 5.f);
        Glow(dl, ImVec2(cx + std::cos(ang) * rx, cy + std::sin(ang) * ry), 2.6f * S(), leds[static_cast<size_t>(i)]);
    }
    for (int i = 0; i < 2; ++i)  // up the right side
        Glow(dl, ImVec2(b.x - w * 0.14f, cy + ry * 0.35f - i * h * 0.12f), 2.6f * S(), leds[static_cast<size_t>(6 + i)]);
}

// The ROG Azoth as it's lit: every key from its table (azoth_layout.h: ISO / Nordic, the
// tall Enter), `colors` in that table's order, the control knob and OLED screen top right.
void DrawKeyboard(ImDrawList* dl, ImVec2 a, ImVec2 size, const std::vector<Rgb>& colors) {
    const ImVec2 b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, Hex(0x2A2F38), 9 * S());  // the case
    dl->AddRectFilled(ImVec2(a.x + 2 * S(), a.y + 2 * S()), ImVec2(b.x - 2 * S(), b.y - 2 * S()), Hex(0x171B22), 8 * S());
    const float pad = 7 * S();
    const float u = (size.x - 2 * pad) / 16.f;
    const float rowGap = u * 0.25f;  // the F-row stands apart
    const float gap = std::max(1.5f, u * 0.09f);
    const auto& keys = azoth::IsoKeys();
    for (size_t i = 0; i < keys.size(); ++i) {
        const azoth::Key& k = keys[i];
        const Rgb c = i < colors.size() ? colors[i] : Rgb{50, 54, 64};
        const float x = a.x + pad + k.x * u, y = a.y + pad + k.y * u + (k.y >= 1 ? rowGap : 0);
        const ImVec2 ka(x + gap / 2, y + gap / 2), kb(x + k.w * u - gap / 2, y + k.h * u - gap / 2);
        dl->AddRectFilled(ImVec2(ka.x - 1.5f * S(), ka.y - 1.5f * S()), ImVec2(kb.x + 1.5f * S(), kb.y + 1.5f * S()),
                          Col(c, 150), 3 * S());  // light around the key
        dl->AddRectFilled(ka, kb, Hex(0x20252F), 2.5f * S());  // the keycap
        dl->AddRectFilled(ImVec2(ka.x + u * 0.12f, ka.y + u * 0.08f), ImVec2(kb.x - u * 0.12f, kb.y - u * 0.2f),
                          Col(Scale(c, 0.22), 255), 2 * S());  // its top, lit a little
    }
    // Top right: the control knob, then the OLED screen.
    const float y0 = a.y + pad;
    const ImVec2 knob(a.x + pad + 14.2f * u, y0 + u / 2);
    dl->AddCircleFilled(knob, u * 0.34f, Hex(0x3A414F), 20);
    dl->AddCircle(knob, u * 0.34f, Hex(0x505868), 20, 1 * S());
    const ImVec2 oa(a.x + pad + 14.65f * u, y0 + u * 0.08f), ob(a.x + pad + 16 * u - gap / 2, y0 + u * 0.92f);
    dl->AddRectFilled(oa, ob, Hex(0x05070A), 2 * S());
    if (!colors.empty())
        dl->AddRectFilled(ImVec2(oa.x + u * 0.15f, oa.y + u * 0.3f), ImVec2(oa.x + u * 0.75f, oa.y + u * 0.42f),
                          Col(colors.front(), 160));
}

// The canvas. `selectable`: clicking a device selects it for editing (Manual mode).
// The memory slots drawn filled: as set by hand, else as the system scan says, else a guess.
std::array<bool, 4> RamSlotsShown(Controller& ctl, const SetupHardware& hw) {
    const int mask = ctl.prefs().ramSlots;
    if (mask >= 0) return {(mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0};
    return hw.slotsKnown ? hw.slots : GuessSlots(hw.sticks ? hw.sticks : std::max(ctl.hardware().sticks(), 2));
}

// Which slots hold a stick, for boards whose firmware doesn't say it clearly: A1 A2 B1 B2
// from the CPU outward, as printed on the board.
void RamSlotsCard(Controller& ctl, const Fonts& f) {
    const SetupHardware hw = DetectSetup(ctl.monitor().Snapshot().smbios);
    BeginCard("ramslots");
    CardTitle(f, "Memory slots", Icon::Memory);
    Muted("%s Click a slot to change it (from the CPU outward, as printed on the board).",
          ctl.prefs().ramSlots >= 0 ? "Set by you."
          : hw.slotsKnown         ? "As your board reports them."
                                  : "Your board doesn't say which slots are used, so this is a guess.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    // What the firmware calls each stick's slot (to see why, when it isn't understood).
    {
        std::string names;
        for (const auto& m : ctl.monitor().Snapshot().smbios.memory)
            names += (names.empty() ? "" : ",  ") + ("\"" + m.slot + "\"") + (m.bank.empty() ? "" : " (\"" + m.bank + "\")");
        if (!names.empty()) Muted("Your board names them: %s", names.c_str());
    }
    std::array<bool, 4> slots = RamSlotsShown(ctl, hw);
    static const char* kNames[] = {"A1", "A2", "B1", "B2"};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushID(i);
        char label[24];
        snprintf(label, sizeof label, "%s  %s", kNames[i], slots[static_cast<size_t>(i)] ? "stick" : "empty");
        if (slots[static_cast<size_t>(i)] ? PrimaryButton(label, ImVec2(96 * S(), 0)) : ImGui::Button(label, ImVec2(96 * S(), 0))) {
            slots[static_cast<size_t>(i)] = !slots[static_cast<size_t>(i)];
            ctl.prefs().ramSlots = (slots[0] ? 1 : 0) | (slots[1] ? 2 : 0) | (slots[2] ? 4 : 0) | (slots[3] ? 8 : 0);
            ctl.Changed();
        }
        ImGui::PopID();
    }
    if (ctl.prefs().ramSlots >= 0) {
        ImGui::SameLine(0, 16 * S());
        if (ImGui::Button("As the board says")) {
            ctl.prefs().ramSlots = -1;
            ctl.Changed();
        }
    }
    EndCard();
}

void SetupCanvas(Controller& ctl, UiState& ui, bool selectable) {
    Prefs& prefs = ctl.prefs();
    const float W = ImGui::GetContentRegionAvail().x;
    const float H = std::clamp(W * 0.52f, 300 * S(), 480 * S());
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), Hex(0x0B0E14), 12 * S());
    for (float x = o.x + 20 * S(); x < o.x + W; x += 24 * S())  // dot grid
        for (float y = o.y + 20 * S(); y < o.y + H; y += 24 * S()) dl->AddCircleFilled(ImVec2(x, y), 1 * S(), Hex(0x1C2230), 4);

    // What's there: only what this PC has. Fans with an Aura ARGB header; the board with an
    // Aura controller (or RGB memory, which sits in it).
    const fx::FanLayout& layout = ctl.config().argbFans;
    std::vector<SetupItem> items;
    bool header = false;
    for (const auto& d : ctl.devices()) header |= d.type == 0x00011000;
    if (header)
        for (int i = 0; i < layout.Fans(); ++i) items.push_back({FanItem(i), device::kFans, ImVec2(84 * S(), 84 * S()), i});
    if (!ctl.devices().empty() || HasRgbRam(ctl, ctl.monitor().Snapshot()))
        items.push_back({device::kBoard, device::kBoard, ImVec2(240 * S(), 180 * S())});
    // The board and memory as the system scan found them. The memory is drawn in the board's
    // slots (and selected by clicking the sticks); lit only when RAM lighting is on.
    const SetupHardware hw = DetectSetup(ctl.monitor().Snapshot().smbios);
    const bool ramOn = prefs.ramLighting;
    const std::array<bool, 4> slots = RamSlotsShown(ctl, hw);
    if (prefs.azothKeyboard && HasAzoth(ctl))
        items.push_back({device::kKeyboard, device::kKeyboard, ImVec2(330 * S(), 138 * S())});
    if (prefs.logitechDevices && HasLogitechRgb(ctl))
        items.push_back({device::kMouse, device::kMouse, ImVec2(70 * S(), 112 * S())});

    // Live colors.
    const double t = ImGui::GetTime();
    std::vector<Rgb> fanLeds, boardLeds, mouseLeds(8);
    const fx::Params* fanP = LiveParams(ctl, device::kFans);
    if (fanP) {
        fx::RenderFans(*fanP, t, layout, &fanLeds);
        const double level = LiveLevel(ctl, device::kFans);
        for (Rgb& c : fanLeds) c = Scale(c, level);
    } else {
        fanLeds.assign(static_cast<size_t>(layout.TotalLeds()), Rgb{50, 54, 64});
    }
    const fx::Params* boardP = LiveParams(ctl, device::kBoard);
    const int nBoard = BoardLedCount(ctl);
    boardLeds.resize(static_cast<size_t>(nBoard));
    for (int i = 0; i < nBoard; ++i)
        boardLeds[static_cast<size_t>(i)] = LiveAt(boardP, t, i, nBoard, LiveLevel(ctl, device::kBoard));
    const fx::Params* mouseP = LiveParams(ctl, device::kMouse);
    const bool perLed = ctl.logitech().mouseEffect();  // else the mouse shows one color
    const double mouseLevel = LiveLevel(ctl, device::kMouse);
    for (int i = 0; i < 8; ++i)
        mouseLeds[static_cast<size_t>(i)] = perLed ? LiveAt(mouseP, t, i, 8, mouseLevel) : LiveAt(mouseP, t, 0, 1, mouseLevel);

    // Where: the saved spot, kept inside the canvas.
    auto rectOf = [&](const SetupItem& it) {
        const Spot s = SetupSpot(prefs, it.item);
        float x = o.x + s.x * W - it.size.x / 2, y = o.y + s.y * H - it.size.y / 2;
        x = std::clamp(x, o.x + 6 * S(), o.x + W - it.size.x - 6 * S());
        y = std::clamp(y, o.y + 6 * S(), o.y + H - it.size.y - 22 * S());
        return ImVec2(x, y);
    };

    // The item being dragged goes on top.
    std::stable_sort(items.begin(), items.end(), [&](const SetupItem& a, const SetupItem& b) {
        return (a.item == ui.dragItem) < (b.item == ui.dragItem);
    });
    ImGui::PushID("setup");
    for (const SetupItem& it : items) {
        const ImVec2 a = rectOf(it);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton(it.item.c_str(), it.size);
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3 * S())) {
            ui.dragItem = it.item;
            Spot s = SetupSpot(prefs, it.item);
            // From the item's clamped place, so a drag never starts with a jump.
            s.x = (a.x + it.size.x / 2 - o.x + ImGui::GetIO().MouseDelta.x) / W;
            s.y = (a.y + it.size.y / 2 - o.y + ImGui::GetIO().MouseDelta.y) / H;
            prefs.setupSpots[it.item] = ClampSpot(s);
        }
        // On the board, the memory sticks are their own device.
        ImVec2 ra, rb;
        RamArea(a, it.size, &ra, &rb);
        const bool isBoard = it.device == std::string(device::kBoard);
        auto inRam = [&](ImVec2 m) { return isBoard && ramOn && m.x >= ra.x && m.x <= rb.x && m.y >= ra.y && m.y <= rb.y; };
        const char* target = inRam(ImGui::GetIO().MouseClickedPos[0]) ? device::kRam : it.device;
        if (ImGui::IsItemDeactivated()) {
            if (ui.dragItem == it.item) {
                ui.dragItem.clear();
                ctl.Changed();  // save the layout
            } else if (selectable) {
                ui.lightTarget = ui.lightTarget == target ? "" : target;
            }
        }
        const bool hoverRam = hovered && inRam(ImGui::GetIO().MousePos);
        if (hovered || active) ImGui::SetMouseCursor(active ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);

        const ImVec2 b(a.x + it.size.x, a.y + it.size.y);
        if (it.device == device::kFans) DrawFan(dl, a, it.size, fanLeds, it.fan, layout.LedsPerFan());
        else if (it.device == device::kBoard)
            DrawBoard(dl, a, it.size, boardLeds, hw, slots, ramOn, LiveParams(ctl, device::kRam), t, LiveLevel(ctl, device::kRam));
        else if (it.device == device::kMouse) DrawMouse(dl, a, it.size, mouseLeds);
        else if (it.device == device::kKeyboard) {
            // Key by key, as the keyboard shows it.
            const fx::Params* kp = LiveParams(ctl, device::kKeyboard);
            const double level = LiveLevel(ctl, device::kKeyboard);
            std::vector<Rgb> keys;
            if (kp) keys = azoth::RenderKeys(*kp, t, level);
            else keys.assign(azoth::IsoKeys().size(), LiveAt(kp, t, 0, 1, level));
            DrawKeyboard(dl, a, it.size, keys);
        }

        auto own = [&](const char* id) {
            auto d = prefs.deviceLighting.find(id);
            return d != prefs.deviceLighting.end() && d->second.own;
        };
        const bool selected = selectable && ui.lightTarget == it.device;
        if (selected || (hovered && !hoverRam))
            dl->AddRect(ImVec2(a.x - 4 * S(), a.y - 4 * S()), ImVec2(b.x + 4 * S(), b.y + 4 * S()),
                        Hex(selected ? kAccentHover : kBorder), 10 * S(), 0, (selected ? 2.f : 1.2f) * S());
        if (isBoard && ramOn) {
            // The memory's outline and name, inside the board.
            const bool ramSelected = selectable && ui.lightTarget == device::kRam;
            if (ramSelected || hoverRam)
                dl->AddRect(ImVec2(ra.x - 5 * S(), ra.y - 4 * S()), ImVec2(rb.x + 5 * S(), rb.y + 4 * S()),
                            Hex(ramSelected ? kAccentHover : kBorder), 6 * S(), 0, (ramSelected ? 2.f : 1.2f) * S());
            const char* name = hw.ramName.empty() ? device::Name(device::kRam) : hw.ramName.c_str();
            const ImVec2 ns = ImGui::CalcTextSize(name);
            const ImVec2 np((ra.x + rb.x) / 2 - ns.x / 2, rb.y + 6 * S());
            dl->AddText(np, Hex(ramSelected ? kText : kMuted), name);
            if (own(device::kRam))
                dl->AddCircleFilled(ImVec2(np.x + ns.x + 7 * S(), np.y + ns.y / 2), 3 * S(), Hex(kAccentHover), 12);
        }
        // The name under it; fans are numbered.
        char label[96];
        if (it.fan >= 0) snprintf(label, sizeof label, "Fan %d", it.fan + 1);
        else if (isBoard && !hw.boardName.empty()) snprintf(label, sizeof label, "%s", hw.boardName.c_str());
        else snprintf(label, sizeof label, "%s", device::Name(it.device));
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(a.x + it.size.x / 2 - ts.x / 2, b.y + 3 * S()), Hex(selected ? kText : kMuted), label);
        if (own(it.device)) dl->AddCircleFilled(ImVec2(a.x + it.size.x / 2 + ts.x / 2 + 7 * S(), b.y + 3 * S() + ts.y / 2), 3 * S(),
                                     Hex(kAccentHover), 12);
    }
    ImGui::PopID();
    if (items.empty()) {
        const char* none = "No lights LumaBridge can control were found on this PC yet (Devices > Rescan devices).";
        const ImVec2 ns = ImGui::CalcTextSize(none);
        dl->AddText(ImVec2(o.x + W / 2 - ns.x / 2, o.y + H / 2 - ns.y / 2), Hex(kMuted), none);
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + H));
    ImGui::Dummy(ImVec2(W, 6 * S()));
}

void SetupCard(Controller& ctl, UiState& ui, const Fonts& f, bool selectable) {
    BeginCard("setup");
    CardTitle(f, "Your setup", Icon::Grid);
    Muted(selectable ? "Drag the devices to where they are on your desk. Click one to give it its own lighting; "
                       "a dot next to the name means it has its own."
                     : "Drag the devices to where they are on your desk. Games light them all alike.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    SetupCanvas(ctl, ui, selectable);
    if (ImGui::Button("Reset layout")) {
        ctl.prefs().setupSpots.clear();
        ctl.Changed();
    }
    ImGui::SameLine();
    Muted("Everything LumaBridge found on this PC shows here (Devices lists them all). The memory sits in the "
          "motherboard's slots: click the sticks to select it.");
    EndCard();
}

// Lighting > Manual: your setup on top, then the lighting of everything or one device.
void ManualPage(Controller& ctl, UiState& ui, const Fonts& f) {
    Prefs& p = ctl.prefs();
    SetupCard(ctl, ui, f, true);

    // Which lighting to edit: everything, or one device.
    std::vector<std::string> ids{""};
    std::vector<const char*> labels{"All devices"};
    for (const char* id : device::All()) {
        const sensors::SystemSnapshot snap = ctl.monitor().Snapshot();
        if ((id == std::string(device::kRam) && !(p.ramLighting && HasRgbRam(ctl, snap))) ||
            (id == std::string(device::kMouse) && !(p.logitechDevices && HasLogitechRgb(ctl))) ||
            (id == std::string(device::kKeyboard) && !(p.azothKeyboard && HasAzoth(ctl))) ||
            (id == std::string(device::kOther) && OpenRgbLit(ctl).empty() && LampArrayLit(ctl).empty()))
            continue;
        ids.push_back(id);
        labels.push_back(device::Name(id));
    }
    int sel = 0;
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == ui.lightTarget) sel = static_cast<int>(i);
    ui.lightTarget = ids[static_cast<size_t>(sel)];  // a device that was turned off: back to all
    if (Segmented("target", &sel, labels.data(), static_cast<int>(labels.size()), ImGui::GetContentRegionAvail().x))
        ui.lightTarget = ids[static_cast<size_t>(sel)];
    ImGui::Dummy(ImVec2(0, 6 * S()));

    if (ui.lightTarget.empty()) {
        Look l = MainLook(p);
        if (LookEditor(ctl, ui, f, l)) {
            SetMainLook(p, l);
            ctl.Changed();
        }
    } else {
        DeviceLighting& d = p.deviceLighting[ui.lightTarget];
        const char* name = device::Name(ui.lightTarget);
        BeginCard("device");
        CardTitle(f, name, Icon::Lighting);
        int own = d.own ? 1 : 0;
        const char* modes[] = {"Same as all devices", "Its own lighting"};
        if (Segmented("own", &own, modes, 2, std::min(420 * S(), ImGui::GetContentRegionAvail().x))) {
            if (own && !d.own && d.look == Look{}) d.look = MainLook(p);  // start from what it shows now
            d.own = own == 1;
            ctl.Changed();
        }
        if (ui.lightTarget == device::kKeyboard)
            Muted("The Azoth shows the effect key by key, by cable or through its Omni receiver.");
        else if (ui.lightTarget == device::kOther)
            Muted("Devices with Windows' lighting standard (and OpenRGB's, if you use it) show this lighting, "
                  "across each device from left to right.");
        else if (ui.lightTarget == device::kMouse)
            Muted("%s", ctl.logitech().mouseEffect() ? "It shows the effect LED by LED."
                                                      : "Through G HUB it shows one color: the effect's first LED.");
        else if (!d.own)
            Muted("It shows the same lighting as the rest. Pick \"Its own lighting\" to set it apart.");
        EndCard();
        if (d.own && LookEditor(ctl, ui, f, d.look)) ctl.Changed();
    }
    BrightnessCard(ctl, f, ui.lightTarget);
}

void FansCard(Controller& ctl, const Fonts& f) {
    BeginCard("fans");
    CardTitle(f, "Fans on the ARGB header", Icon::Fan);
    Muted("Per-LED effects (rainbow wave, gradient, comet, twinkle) need to know how the LEDs are "
          "grouped. Fans chained on a hub count in order. The fan's box or product page says how many LEDs it "
          "has; the test pattern below confirms it.");
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
    if (Toggle("Show test pattern", &test)) ctl.SetFanTest(test);
    ImGui::SameLine();
    Muted("Each fan should be one solid color with a single white LED. If a color spills onto the "
          "next fan, change LEDs per fan.");
    if (ctl.fanTest()) {
        ImGui::Dummy(ImVec2(0, 4 * S()));
        LightsPreview(ctl.output(), l, 0);
    }
    EndCard();
}

// ---- Devices: status and settings of each -------------------------------------------

struct DeviceStatus {
    DeviceStatus(std::string t = {}, unsigned c = kMuted, std::string why = {})
        : text(std::move(t)), color(c), tip(std::move(why)) {}
    std::string text;
    unsigned color;
    std::string tip;  // why, shown on hover
};

DeviceStatus OpenRgbStatus(const Controller& ctl, const OpenRgbDevice& d) {
    if (!ctl.OpenRgbOn(d))
        return ctl.OpenRgbDefaultOn(d) ? DeviceStatus{"Off", kMuted}
                                       : DeviceStatus{"Lit by LumaBridge itself", kMuted,
                                                      "LumaBridge lights this device directly, so it's left out here."};
    return ctl.output().stopped ? DeviceStatus{"Its own effect", kMuted} : DeviceStatus{"Following LumaBridge", kGreen};
}

DeviceStatus LampArrayStatus(const Controller& ctl, const LampArrayDevice& d) {
    if (!d.problem.empty()) return {"Can't light it", kAmber, d.problem};
    if (!ctl.LampArrayOn(d))
        return ctl.LampArrayDefaultOn(d) ? DeviceStatus{"Off", kMuted}
                                         : DeviceStatus{"Lit by LumaBridge itself", kMuted,
                                                        "LumaBridge lights this device another way, so it's left out here."};
    return ctl.output().stopped ? DeviceStatus{"Its own effect", kMuted} : DeviceStatus{"Following LumaBridge", kGreen};
}

DeviceStatus LogitechStatus(Controller& ctl) {
    const auto& lg = ctl.logitech();
    using S_ = LogitechOutput::State;
    if (!ctl.prefs().logitechDevices) return {"Off", kMuted};
    if (ctl.logitechAsleep() && lg.state() == LogitechOutput::State::Active)
        return {"Asleep - not used for a while", kMuted, "It lights up again when you use it."};
    switch (lg.state()) {
    case S_::Active: return {lg.mouseEffect() ? "Following LumaBridge, every LED" : "Following LumaBridge", kGreen};
    case S_::NoGHub: return {"G HUB not found", kRed};
    case S_::Waiting: return {"Waiting for G HUB", kAmber};
    default: return {"G HUB has them", kMuted, ctl.logitechNote()};
    }
}

DeviceStatus AzothStatus(Controller& ctl) {
    const auto& az = ctl.azoth();
    using A_ = AzothOutput::State;
    if (!ctl.prefs().azothKeyboard) return {"Off", kMuted};
    if (ctl.azothAsleep() && az.state() == AzothOutput::State::Active)
        return {"Asleep - not used for a while", kMuted, "It lights up again with the next key press."};
    switch (az.state()) {
    case A_::Active: return {az.wireless() ? "Following LumaBridge, every key (wireless)" : "Following LumaBridge, every key", kGreen};
    case A_::NotFound: return {"Not connected", kAmber};
    default: return {"Armoury Crate's lighting", kMuted};
    }
}

// "Fade out when not used": the switch and the delay (seconds, shown in minutes from 1 minute).
bool SleepControls(const char* id, bool* on, int* seconds) {
    ImGui::PushID(id);
    bool changed = Toggle("Fade out when you're not using it", on);
    if (*on) {
        ImGui::SameLine(0, 18 * S());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("after");
        ImGui::SameLine();
        static const int kSteps[] = {30, 60, 120, 300, 600, 900, 1800};
        static const char* kNames[] = {"30 s", "1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
        int sel = 1;
        for (int i = 0; i < 7; ++i)
            if (kSteps[i] <= *seconds) sel = i;
        ImGui::SetNextItemWidth(110 * S());
        if (ImGui::Combo("##after", &sel, kNames, 7)) {
            *seconds = kSteps[sel];
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

void LogitechCard(Controller& ctl, const Fonts& f) {
    const auto& lg = ctl.logitech();
    BeginCard("logitech");
    CardTitle(f, "Settings", Icon::Gear);
    Muted("Your Logitech RGB gear shows LumaBridge's lighting through Logitech's own LED SDK in G HUB "
          "(nothing goes into a game), as one color: the first LED of the effect. A mouse LumaBridge knows LED by "
          "LED (the G502 X Plus so far) shows the whole effect instead, and runs breathing, color cycle and the "
          "rainbow wave itself.");
    if (ctl.prefs().logitechDevices && lg.state() == LogitechOutput::State::Released && !ctl.logitechNote().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, V4(kAmber));
        ImGui::TextWrapped("Right now G HUB has them: %s.", ctl.logitechNote().c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 2 * S()));
    bool enabled = ctl.prefs().logitechDevices;
    if (Toggle("Light Logitech devices", &enabled)) ctl.SetLogitechEnabled(enabled);
    if (ImGui::IsItemHovered() && !lg.dllPath().empty()) ImGui::SetTooltip("%s", Utf8(lg.dllPath()).c_str());
    bool force = ctl.prefs().logitechForce;
    if (Toggle("Keep them with LumaBridge, even when a game lights them", &force)) {
        ctl.prefs().logitechForce = force;
        ctl.Changed();
    }
    Muted("Off (the default): while a game lights Logitech gear itself (LIGHTSYNC through G HUB, like Battlefield), "
          "LumaBridge hands it to the game, so the mouse shows the game's own effects. On: your Logitech gear follows "
          "LumaBridge like the rest, games included.");
    ImGui::Dummy(ImVec2(0, 6 * S()));
    if (SleepControls("logisleep", &ctl.prefs().logitechSleep, &ctl.prefs().logitechSleepSec)) ctl.Changed();
    const auto gh = ctl.ghubSleep();
    Muted("Like G HUB: when you haven't used it for a while, its lighting fades out and LumaBridge stops sending, so a "
          "wireless mouse can sleep; it lights up again when you use it. Whether it fades follows G HUB's \"turn off "
          "lighting on inactivity\" (%s).",
          !gh ? "G HUB didn't answer, so on" : *gh ? "on in G HUB" : "off in G HUB, so it stays lit");
    EndCard();
}

void AzothCard(Controller& ctl, const Fonts& f) {
    BeginCard("azoth");
    CardTitle(f, "Settings", Icon::Gear);
    Muted("Every key shows LumaBridge's effect on its own, so waves and gradients run across the keyboard, by cable "
          "or through its ROG Omni receiver (a little slower there, to spare the battery). LumaBridge never sends "
          "Armoury Crate's save command, so your saved Armoury Crate lighting stays in the keyboard. When LumaBridge lets go, the "
          "keyboard keeps the last colors until it restarts or Armoury Crate sets it again.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    bool enabled = ctl.prefs().azothKeyboard;
    if (Toggle("Light the ROG Azoth", &enabled)) ctl.SetAzothEnabled(enabled);
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (SleepControls("azothsleep", &ctl.prefs().azothSleep, &ctl.prefs().azothSleepSec)) ctl.Changed();
    Muted("When you haven't typed for a while, the keys fade out and LumaBridge stops sending, so the keyboard can "
          "sleep (and save its battery wirelessly). The next key press lights it up again.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Run the device probe")) {
        const std::wstring exe = AppDirectory() + L"\\tools\\device-probe.exe";
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    EndCard();
}

// Result of the last Set up / Remove of the hardware helper, under its button.
void HelperMessage(Integrations& in) {
    if (in.LastId() != "helper") return;
    const std::string msg = in.LastMessage();
    if (msg.empty()) return;
    const bool bad = msg.rfind("Failed", 0) == 0 || msg.rfind("Could not", 0) == 0 || msg.rfind("Cancelled", 0) == 0;
    ImGui::PushStyleColor(ImGuiCol_Text, V4(in.Busy() ? kAmber : bad ? kRed : kGreen));
    ImGui::TextWrapped("%s", msg.c_str());
    ImGui::PopStyleColor();
}

const Integration* HelperSetup(Controller& ctl, Integrations& in, UiState& ui) {
    EnsureIntegrations(ctl, in, ui);
    for (const auto& it : in.list())
        if (it.id == "helper") return &it;
    return nullptr;
}

void SetUpHelperButton(Integrations& in, const Integration* setup) {
    ImGui::BeginDisabled(in.Busy());
    const bool update = setup && setup->state == IntegrationState::Problem;
    if (PrimaryButton(in.Busy() ? "Setting up..." : update ? "Update (administrator, once)" : "Set up (administrator, once)"))
        in.Install("helper");
    ImGui::EndDisabled();
}

void HardwareCard(Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    const Integration* setup = HelperSetup(ctl, in, ui);
    const bool setUp = setup && setup->state == IntegrationState::Active;
    const auto& hw = ctl.hardware();
    using H = HardwareHelper::State;
    const H st = hw.state();

    BeginCard("hardware");
    IconItem(Icon::Gear, 18 * S(), Hex(setUp && st == H::Running ? kAccent : kMuted));
    ImGui::SameLine(0, 10 * S());
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted("Hardware access");
    ImGui::PopFont();
    ImGui::SameLine(0, 12 * S());
    if (!setUp) Pill(setup && setup->state == IntegrationState::Problem ? "Update needed" : "Not set up", kAmber);
    else if (st == H::Running) Pill("Ready", kGreen);
    else if (st == H::NoPawnIO) Pill("Driver missing", kRed);
    else Pill("Starting...", kAmber);
    Muted("RAM lighting, fan speeds and CPU / board temperatures need access Windows only gives to administrators. "
          "One click sets it up: LumaBridge installs PawnIO (a small signed driver made for this, included with "
          "LumaBridge) and its own helper, which LumaBridge then starts by itself. Nothing else to download.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (setUp && st == H::Running) {
        const auto sensors = hw.Sensors();
        int fans = 0, temps = 0;
        for (const auto& x : sensors) (x.type == sensors::SensorType::Fan ? fans : temps)++;
        const std::string chip = hw.chip();
        Muted("Reading %d temperature(s) and %d fan(s)%s.", temps, fans,
              chip.empty() ? "" : (" from the board's " + chip).c_str());
    } else if (setUp && st == H::NoPawnIO) {
        Muted("The PawnIO driver isn't installed any more. Set up again to reinstall it.");
    }
    if (!setUp || st == H::NoPawnIO) {
        SetUpHelperButton(in, setup);
    } else {
        ImGui::BeginDisabled(in.Busy());
        if (ImGui::SmallButton("Set up again")) in.Install("helper");
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) in.Remove("helper");
        ImGui::EndDisabled();
    }
    HelperMessage(in);
    EndCard();
}

DeviceStatus RamStatus(Controller& ctl, bool setUp) {
    const auto& hw = ctl.hardware();
    using R = HardwareHelper::RamState;
    if (!ctl.prefs().ramLighting) return {"Off", kMuted};
    if (!setUp) return {"Needs hardware access", kAmber};
    switch (hw.ramState()) {
    case R::Active: {
        char b[48];
        snprintf(b, sizeof b, "Following LumaBridge (%d stick%s)", hw.sticks(), hw.sticks() == 1 ? "" : "s");
        return {b, kGreen};
    }
    case R::Problem: return {"Can't light the RAM", kRed};
    case R::Released: return {"Armoury Crate's lighting", kMuted};
    default: return {"Starting...", kAmber};
    }
}

void MemoryCard(Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    const Integration* setup = HelperSetup(ctl, in, ui);
    const bool setUp = setup && setup->state == IntegrationState::Active;
    const auto& hw = ctl.hardware();
    const bool on = ctl.prefs().ramLighting;

    BeginCard("memory");
    CardTitle(f, "Settings", Icon::Gear);
    Muted("HyperX / Kingston FURY RGB DDR4 sticks show LumaBridge's effect across their five LEDs (AMD chipsets). "
          "LumaBridge only ever writes the sticks' lighting registers, never their configuration chip. If the sticks "
          "flicker, Armoury Crate is lighting them too: switch the RAM off in Armoury Crate.");
    if (on && setUp && hw.ramState() == HardwareHelper::RamState::Problem) {
        ImGui::PushStyleColor(ImGuiCol_Text, V4(kRed));
        ImGui::TextWrapped("%s", hw.ramProblem().c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 2 * S()));
    ImGui::TextUnformatted("When LumaBridge lets go (switched off, Armoury Crate's turn, exit)");
    static const char* kRelease[] = {"Their own rainbow", "Off", "Keep the last color"};
    int release = std::clamp(ctl.prefs().ramRelease, 0, 2);
    if (Segmented("ramrelease", &release, kRelease, 3, std::min(520 * S(), ImGui::GetContentRegionAvail().x))) {
        ctl.prefs().ramRelease = release;
        ctl.Changed();
    }
    bool enabled = on;
    if (Toggle("Light the RAM", &enabled)) {
        ctl.SetRamEnabled(enabled);
        if (enabled && !setUp && !in.Busy()) in.Install("helper");  // one click: switching it on sets it up
    }
    if (on && !setUp) {
        ImGui::SameLine();
        SetUpHelperButton(in, setup);
    }
    HelperMessage(in);
    EndCard();
}

// Aura devices are switched on and off by name (Config::auraDisabledDevices).
bool AuraDeviceOn(Controller& ctl, const std::wstring& name) {
    const auto& off = ctl.config().auraDisabledDevices;
    return std::none_of(off.begin(), off.end(), [&](const std::wstring& n) { return _wcsicmp(n.c_str(), name.c_str()) == 0; });
}

void SetAuraDeviceOn(Controller& ctl, const std::wstring& name, bool on) {
    auto& off = ctl.config().auraDisabledDevices;
    off.erase(std::remove_if(off.begin(), off.end(), [&](const std::wstring& n) { return _wcsicmp(n.c_str(), name.c_str()) == 0; }),
              off.end());
    if (!on) off.push_back(name);
    ctl.Changed();
}

// Which lighting an Aura device shows: the ARGB header the fans', the rest the board's.
const char* AuraLightingId(const AuraDeviceInfo& d) { return d.type == 0x00011000 ? device::kFans : device::kBoard; }

DeviceStatus AuraStatus(Controller& ctl, const AuraDeviceInfo& d) {
    if (!AuraDeviceOn(ctl, d.name)) return {"Switched off", kMuted};
    return ctl.auraStatus().connected ? DeviceStatus{"Following LumaBridge", kGreen}
                                      : DeviceStatus{"Armoury Crate's lighting", kMuted};
}

// The top of a device's page: a way back, its name and how it's doing.
bool DeviceHeader(UiState& ui, const Fonts& f, Icon icon, const std::string& name, const DeviceStatus& st,
                  bool experimental, const std::string& detail) {
    if (ImGui::Button("<  All devices")) {
        ui.deviceDetail.clear();
        return false;
    }
    ImGui::Dummy(ImVec2(0, 2 * S()));
    BeginCard("device-head");
    IconItem(icon, 26 * S(), Hex(st.color == kGreen ? kAccent : kMuted));
    ImGui::SameLine(0, 12 * S());
    ImGui::BeginGroup();
    ImGui::PushFont(f.title);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopFont();
    Pill(st.text.c_str(), st.color);
    if (experimental) {
        ImGui::SameLine();
        Pill("Experimental", kAccent);
    }
    if (!detail.empty()) Muted("%s", detail.c_str());
    ImGui::EndGroup();
    EndCard();
    return true;
}

// The device's lighting, and the way to change it on the Lighting page.
void DeviceLightingCard(Controller& ctl, UiState& ui, const Fonts& f, const char* id) {
    const Prefs& p = ctl.prefs();
    auto it = p.deviceLighting.find(id);
    const bool own = it != p.deviceLighting.end() && it->second.own;
    BeginCard("device-lighting");
    CardTitle(f, "Lighting", Icon::Lighting);
    if (own) {
        const Look l = it->second.look;
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = std::min(260 * S(), ImGui::GetContentRegionAvail().x);
        EffectStrip(ToParams(l), a, ImVec2(a.x + w, a.y + 16 * S()), 8 * S());
        ImGui::Dummy(ImVec2(w, 16 * S()));
        Muted("Its own lighting: %s.", kEffects[static_cast<int>(l.effect)].name);
    } else {
        Muted("It shows the main lighting, like the other devices.");
    }
    Muted("Brightness: %.0f%% of the overall %.0f%%.", DeviceBrightness(p, id) * 100.0,
          ctl.config().auraCorrection.brightness * 100.0);
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (PrimaryButton("Change its lighting")) {
        ui.page = Page::Lighting;
        ui.lightTarget = id;
    }
    if (p.mode == Mode::Auto)
        Muted("Its own lighting and colors are set in Manual mode; games light every device alike.");
    EndCard();
}

// A device's own page.
void DeviceDetailPage(Controller& ctl, Integrations& in, UiState& ui, const Fonts& f,
                      const sensors::SystemSnapshot& snap) {
    const std::string& id = ui.deviceDetail;
    if (id.rfind("aura:", 0) == 0) {
        const auto& devs = ctl.devices();
        const AuraDeviceInfo* d = nullptr;
        for (const auto& x : devs)
            if ("aura:" + Utf8(x.name) == id) d = &x;
        if (!d) {
            if (ImGui::Button("<  All devices")) ui.deviceDetail.clear();
            Muted("This device isn't there any more. Rescan devices on the list.");
            return;
        }
        Icon icon;
        const std::string label = DeviceLabel(*d, snap, ctl.config().argbFans, &icon);
        char type[64];
        WideCharToMultiByte(CP_UTF8, 0, AuraDeviceTypeName(d->type), -1, type, sizeof type, nullptr, nullptr);
        const std::string detail = "Aura " + std::string(type) + ", " + std::to_string(d->lightCount) + " LEDs (" +
                                   Utf8(d->name) + ")";
        if (!DeviceHeader(ui, f, icon, label, AuraStatus(ctl, *d), false, detail)) return;

        BeginCard("aura-device");
        CardTitle(f, "Settings", Icon::Gear);
        bool on = AuraDeviceOn(ctl, d->name);
        if (Toggle("Light this device", &on)) SetAuraDeviceOn(ctl, d->name, on);
        Muted("Switched off, it stays dark while LumaBridge controls the lights.");
        EndCard();
        if (d->type == 0x00011000) FansCard(ctl, f);
        DeviceLightingCard(ctl, ui, f, AuraLightingId(*d));
        return;
    }
    if (id == device::kRam) {
        const Integration* setup = HelperSetup(ctl, in, ui);
        const bool setUp = setup && setup->state == IntegrationState::Active;
        const SetupHardware hw = DetectSetup(snap.smbios);
        std::string detail = hw.ramName.empty() ? "HyperX / Kingston FURY RGB DDR4" : hw.ramName;
        if (hw.sticks) detail += ", " + std::to_string(hw.sticks) + " stick" + (hw.sticks == 1 ? "" : "s") + " found";
        if (!DeviceHeader(ui, f, Icon::Memory, "Memory (RAM)", RamStatus(ctl, setUp), true, detail)) return;
        MemoryCard(ctl, in, ui, f);
        RamSlotsCard(ctl, f);
        HardwareCard(ctl, in, ui, f);
        DeviceLightingCard(ctl, ui, f, device::kRam);
        return;
    }
    if (id == device::kMouse) {
        if (!DeviceHeader(ui, f, Icon::Mouse, LogitechName(ctl), LogitechStatus(ctl), false,
                          LogitechKinds(ctl) + ", through G HUB"))
            return;
        LogitechCard(ctl, f);
        DeviceLightingCard(ctl, ui, f, device::kMouse);
        return;
    }
    if (id == device::kKeyboard) {
        if (!DeviceHeader(ui, f, Icon::Keyboard, "ASUS ROG Azoth", AzothStatus(ctl), true,
                          "By cable, or wirelessly through its ROG Omni receiver"))
            return;
        AzothCard(ctl, f);
        DeviceLightingCard(ctl, ui, f, device::kKeyboard);
        return;
    }
    if (id.rfind("lamparray:", 0) == 0) {
        const std::string name = id.substr(10);
        for (const auto& d : ctl.lampArray().devices()) {
            if (d.name != name) continue;
            char vidpid[16];
            snprintf(vidpid, sizeof vidpid, "%04X:%04X", d.vid, d.pid);
            const std::string detail = std::string(lamparray::KindName(d.kind)) + ", " + std::to_string(d.lamps) +
                                       " lamps, Windows lighting standard (USB " + vidpid + ")";
            if (!DeviceHeader(ui, f, Icon::Leds, d.name, LampArrayStatus(ctl, d), false, detail)) return;
            BeginCard("lamparray-device");
            CardTitle(f, "Settings", Icon::Gear);
            if (!d.problem.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, V4(kAmber));
                ImGui::TextWrapped("%s.", d.problem.c_str());
                ImGui::PopStyleColor();
                Muted("If Windows' own Dynamic Lighting (Settings > Personalization > Dynamic Lighting) controls it, "
                      "turn that off for this device.");
            } else {
                bool on = ctl.LampArrayOn(d);
                if (Toggle("Light it", &on)) {
                    ctl.prefs().lampArrayDevices[d.name] = on;
                    ctl.Changed();
                }
                if (!ctl.LampArrayDefaultOn(d))
                    Muted("LumaBridge lights this device another way, so it's off here by default: both at once would "
                          "fight.");
                else
                    Muted("Switched off, it runs its own effect again. If Windows' own Dynamic Lighting (Settings > "
                          "Personalization > Dynamic Lighting) also lights it, turn that off so the two don't take turns.");
            }
            EndCard();
            DeviceLightingCard(ctl, ui, f, device::kOther);
            return;
        }
        if (ImGui::Button("<  All devices")) ui.deviceDetail.clear();
        Muted("This device isn't there any more.");
        return;
    }
    if (id.rfind("openrgb:", 0) == 0) {
        const std::string name = id.substr(8);
        for (const auto& d : ctl.openRgb().devices()) {
            if (d.name != name) continue;
            const std::string detail = std::string(openrgb::TypeName(d.type)) + (d.vendor.empty() ? "" : " by " + d.vendor) +
                                       ", " + std::to_string(d.leds) + " LEDs, through OpenRGB";
            if (!DeviceHeader(ui, f, Icon::Leds, d.name, OpenRgbStatus(ctl, d), false, detail)) return;
            BeginCard("openrgb-device");
            CardTitle(f, "Settings", Icon::Gear);
            bool on = ctl.OpenRgbOn(d);
            if (Toggle("Light it through OpenRGB", &on)) {
                ctl.prefs().openRgbDevices[d.name] = on;
                ctl.Changed();
            }
            if (!ctl.OpenRgbDefaultOn(d))
                Muted("LumaBridge lights this device itself, so it's off here by default: both at once would fight.");
            else
                Muted("Switched off, it shows its own effect again (the mode it had in OpenRGB).");
            EndCard();
            DeviceLightingCard(ctl, ui, f, device::kOther);
            return;
        }
        if (ImGui::Button("<  All devices")) ui.deviceDetail.clear();
        Muted("OpenRGB doesn't list this device any more.");
        return;
    }
    ui.deviceDetail.clear();
}

void DevicesPage(Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    const sensors::SystemSnapshot snap = ctl.monitor().Snapshot();  // board name for the labels
    // The fan test pattern belongs to the fans' page.
    const bool fanPage = [&] {
        for (const auto& d : ctl.devices())
            if (d.type == 0x00011000 && ui.deviceDetail == "aura:" + Utf8(d.name)) return true;
        return false;
    }();
    if (!fanPage && ctl.fanTest()) ctl.SetFanTest(false);
    if (!ui.deviceDetail.empty()) {
        DeviceDetailPage(ctl, in, ui, f, snap);
        return;
    }
    auto st = ctl.auraStatus();
    const auto& aura = ctl.devices();

    BeginCard("status");
    CardTitle(f, "Aura connection", Icon::Plug);
    if (st.connected) {
        Pill("Connected", kGreen);
        ImGui::SameLine();
        Muted("%d device(s) under LumaBridge control.", static_cast<int>(st.devices.size()));
    } else if (!st.running) {
        Pill("Not controlling the lights", kMuted);
        ImGui::SameLine();
        Muted("%d device(s) found. LumaBridge takes over when a game or your manual color needs the "
              "lights.", static_cast<int>(aura.size()));
    } else {
        Pill("Aura controller not found", kRed);
        ImGui::SameLine();
        Muted("LumaBridge couldn't find the motherboard's Aura USB controller. Details are in the log.");
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (ImGui::Button("Rescan devices")) ctl.RescanDevices();
    EndCard();

    // Every device, like the games list: click one for its settings.
    struct Row {
        std::string id, name, kind;
        Icon icon;
        DeviceStatus status;
        int leds = -1;
    };
    std::vector<Row> rows;
    for (const auto& d : aura) {
        Row r;
        r.id = "aura:" + Utf8(d.name);
        r.name = DeviceLabel(d, snap, ctl.config().argbFans, &r.icon);
        char type[64];
        WideCharToMultiByte(CP_UTF8, 0, AuraDeviceTypeName(d.type), -1, type, sizeof type, nullptr, nullptr);
        r.kind = std::string("Aura ") + type;
        r.status = AuraStatus(ctl, d);
        r.leds = d.lightCount;
        rows.push_back(r);
    }
    if (HasRgbRam(ctl, snap)) {
        const Integration* setup = HelperSetup(ctl, in, ui);
        const SetupHardware hw = DetectSetup(snap.smbios);
        rows.push_back({device::kRam, "Memory (RAM)", hw.ramName.empty() ? "Memory" : hw.ramName, Icon::Memory,
                        RamStatus(ctl, setup && setup->state == IntegrationState::Active),
                        ctl.prefs().ramLighting && ctl.hardware().sticks() ? ctl.hardware().sticks() * 5 : -1});
    }
    if (HasLogitechRgb(ctl))
        rows.push_back({device::kMouse, LogitechName(ctl), LogitechKinds(ctl) + ", through G HUB", Icon::Mouse,
                        LogitechStatus(ctl)});
    if (HasAzoth(ctl)) rows.push_back({device::kKeyboard, "ASUS ROG Azoth", "Keyboard", Icon::Keyboard, AzothStatus(ctl)});
    if (ctl.prefs().lampArray)
        for (const auto& d : ctl.lampArray().devices())
            rows.push_back({"lamparray:" + d.name, d.name, std::string(lamparray::KindName(d.kind)) + ", Windows lighting standard",
                            Icon::Leds, LampArrayStatus(ctl, d), d.lamps ? static_cast<int>(d.lamps) : -1});
    if (ctl.prefs().openRgb)
        for (const auto& d : ctl.openRgb().devices())
            rows.push_back({"openrgb:" + d.name, d.name, std::string(openrgb::TypeName(d.type)) + ", through OpenRGB",
                            Icon::Leds, OpenRgbStatus(ctl, d), static_cast<int>(d.leds)});
    // Found, but nothing LumaBridge can light: said, so it isn't a mystery.
    std::string unlit;
    for (const auto& d : ctl.presence().logitech)
        if (!d.rgb && !d.name.empty())
            unlit += (unlit.empty() ? "" : ", ") + ("Logitech " + d.name) + " (" + hidpp::DeviceTypeName(d.type) + ")";

    BeginCard("devices");
    CardTitle(f, "Devices", Icon::Leds);
    Muted("Click a device for its settings and lighting.%s",
          aura.empty() ? " No Aura devices found: click Rescan devices; if it stays empty, the log (Settings) says why." : "");
    if (!unlit.empty()) Muted("Also found, without RGB lighting: %s.", unlit.c_str());
    if (const auto also = AlsoFound(ctl, snap); !also.empty() && ctl.openRgb().state() != OpenRgbOutput::State::Connected)
        Muted("Also found: %s. LumaBridge can't light these directly yet: they don't use Windows' lighting standard. "
              "Their own app lights them (or OpenRGB, if you use it: Integrations).",
              Join(also).c_str());
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (rows.empty()) Muted("Nothing LumaBridge can light was found on this PC yet.");
    else if (ImGui::BeginTable("devices", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 2.6f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("LEDs", ImGuiTableColumnFlags_WidthFixed, 50 * S());
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24 * S());
        ImGui::TableHeadersRow();
        int n = 0;
        for (const Row& r : rows) {
            ImGui::PushID(n++);
            ImGui::TableNextRow(0, 34 * S());
            ImGui::TableNextColumn();
            // The whole row opens the device's page.
            ImGui::PushStyleColor(ImGuiCol_Header, V4(kAccent, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, V4(kAccent, 0.14f));
            const bool open = ImGui::Selectable("##row", false,
                                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                                ImVec2(0, 26 * S()));
            ImGui::PopStyleColor(2);
            ImGui::SameLine(0, 0);
            ImGui::AlignTextToFramePadding();  // the icon and name on the row's text line
            IconItem(r.icon, 16 * S(), r.status.color == kGreen ? Hex(kAccent) : Hex(kMuted));
            ImGui::SameLine();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            Muted("%s", r.kind.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            Pill(r.status.text.c_str(), r.status.color);
            if (!r.status.tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.status.tip.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (r.leds >= 0) Muted("%d", r.leds);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            Muted(">");
            ImGui::PopID();
            if (open) ui.deviceDetail = r.id;
        }
        ImGui::EndTable();
    }
    EndCard();

    HardwareCard(ctl, in, ui, f);
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

// Dynamic-lighting status of an installed game, for the list and its page.
struct LightingStatus {
    const char* text;
    unsigned color;
    bool pill;
    std::string tip;
};

LightingStatus GameLighting(Controller& ctl, const InstalledGame& g, const Controller::GameStatus* running,
                            const games::GameProfile* profile) {
    using games::ProfileKind;
    const auto& seen = ctl.prefs().lightingGames;
    bool hasSeen = false;
    for (const auto& exe : g.exeNames) hasSeen |= std::find(seen.begin(), seen.end(), exe) != seen.end();
    const std::string about = profile ? std::string(profile->how) + ".\n" + profile->note : std::string();
    if (running && running->support == games::Support::Active) return {"Lighting now", kGreen, true, ""};
    if (profile && profile->kind == ProfileKind::NotAGame) return {"Not a game", kMuted, false, about};
    if (profile && profile->kind == ProfileKind::BuiltIn) return {"Built in", kGreen, true, about};
    if (hasSeen || (running && games::SupportsLighting(running->support)))
        return {"Supported", kGreen, true, "LumaBridge has seen this game send lighting."};
    if (profile && profile->kind == ProfileKind::VendorSdk) return {profile->how, kAmber, true, about};
    if (!g.sdk.empty())
        return {"Likely", kAmber, true,
                "The game's folder has " + g.sdk + " files. It shows as Supported once it sends lighting."};
    if (profile && profile->kind == ProfileKind::NoSupport) return {"No lighting support", kMuted, false, about};
    return {"Not detected", kMuted, false,
            "No lighting SDK files in its folder and no lighting seen yet. Some games build the SDK in, so "
            "play it once with LumaBridge running to be sure."};
}

const games::GameProfile* ProfileFor(const InstalledGame* g, const Controller::GameStatus* running, const std::string& name) {
    const games::GameProfile* profile = running ? running->profile : nullptr;
    for (size_t i = 0; g && !profile && i < g->exeNames.size(); ++i) profile = games::FindProfile(g->exeNames[i], "");
    if (!profile) profile = games::FindProfile("", name);
    return profile;
}

const char* kGameModeNames[] = {"Default", "Screen colors", "My idle choice", "Own color"};

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

// Re-reads whether the built-in feeds are set up (every couple of seconds).
void RefreshFeeds(Controller& ctl, UiState& ui) {
    const uint64_t now = GetTickCount64();
    if (now < ui.feedCheckAt) return;
    ui.feedCheckAt = now + 2000;
    ui.cs2Dir = ctl.GameDir("cs2");
    ui.rlDir = ctl.GameDir("rocketleague");
    ui.cs2Installed = Cs2ConfigInstalled(ui.cs2Dir);
    ui.dotaDir = ctl.GameDir("dota2");
    ui.dotaInstalled = Dota2ConfigInstalled(ui.dotaDir);
    ui.rlIniFound = !RocketLeagueStatsText(ui.rlDir, true).empty();
    ui.rlEnabled = RocketLeagueStatsEnabled(ui.rlDir);
    ctl.RefreshFeedSettings();
}

// Status of a built-in feed as a pill.
void FeedPill(const Controller& ctl, const UiState& ui, const std::string& key) {
    const auto& feeds = ctl.feeds();
    if (key == "cs2") {
        if (!feeds.Cs2Listening()) Pill("Port busy", kRed);
        else if (feeds.Cs2Seen()) Pill("Receiving", kGreen);
        else Pill(ui.cs2Installed ? "Set up" : "Not set up", ui.cs2Installed ? kGreen : kAmber);
    } else if (key == "rocketleague") {
        if (feeds.RocketLeagueConnected()) Pill("Receiving", kGreen);
        else Pill(ui.rlEnabled ? "Switched on" : "Off", ui.rlEnabled ? kGreen : kAmber);
    } else if (key == "dota2") {
        if (!feeds.Cs2Listening()) Pill("Port busy", kRed);
        else if (feeds.Dota2Seen()) Pill("Receiving", kGreen);
        else Pill(ui.dotaInstalled ? "Set up" : "Not set up", ui.dotaInstalled ? kGreen : kAmber);
    } else if (key == "league") {
        Pill(feeds.LeagueSeen() ? "Receiving" : "Ready", kGreen);
    } else if (key == "forza") {
        if (feeds.ForzaPortBusy()) Pill("Port busy", kRed);
        else Pill(feeds.ForzaSeen() ? "Receiving" : "Switch on in the game", feeds.ForzaSeen() ? kGreen : kAmber);
    } else {
        Pill(feeds.WarThunderSeen() ? "Receiving" : "Ready", kGreen);
    }
}

// Setting up a built-in feed (on the game's page).
void FeedSetup(Controller& ctl, Integrations& in, UiState& ui, const std::string& key) {
    const auto& feeds = ctl.feeds();
    if (key == "cs2") {
        if (ui.cs2Dir.empty()) {
            Muted("Counter-Strike 2 wasn't found in your game libraries (Rescan on the list).");
        } else if (ui.cs2Installed) {
            Muted("%s", feeds.Cs2Seen() ? "CS2 is sending its game state." : "Set up. Restart CS2 once so it picks it up.");
            ImGui::BeginDisabled(in.Busy());
            if (ImGui::Button("Remove##cs2"))
                WriteFeedFile(in, ui, "cs2", "Counter-Strike 2 feed removed", Cs2ConfigPath(ui.cs2Dir), "", true);
            ImGui::EndDisabled();
        } else {
            ImGui::BeginDisabled(in.Busy());
            if (PrimaryButton("Set up##cs2"))
                WriteFeedFile(in, ui, "cs2", "Counter-Strike 2 feed set up - restart CS2", Cs2ConfigPath(ui.cs2Dir),
                              Cs2ConfigText(), false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            Muted("Adds gamestate_integration_lumabridge.cfg to CS2's cfg folder (Valve's official way).");
        }
    } else if (key == "rocketleague") {
        const bool on = ui.rlEnabled;
        if (ui.rlDir.empty()) {
            Muted("Rocket League wasn't found in your game libraries (Rescan on the list).");
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
    } else if (key == "dota2") {
        if (ui.dotaDir.empty()) {
            Muted("Dota 2 wasn't found in your game libraries (Rescan on the list).");
        } else if (ui.dotaInstalled) {
            Muted("%s", feeds.Dota2Seen() ? "Dota 2 is sending its game state."
                                          : "Set up. In Steam, add -gamestateintegration to Dota 2's launch options "
                                            "(Properties > General), then restart Dota 2.");
            ImGui::BeginDisabled(in.Busy());
            if (ImGui::Button("Remove##dota2"))
                WriteFeedFile(in, ui, "dota2", "Dota 2 feed removed", Dota2ConfigPath(ui.dotaDir), "", true);
            ImGui::EndDisabled();
        } else {
            ImGui::BeginDisabled(in.Busy());
            if (PrimaryButton("Set up##dota2"))
                WriteFeedFile(in, ui, "dota2", "Dota 2 feed set up - add -gamestateintegration to its launch options",
                              Dota2ConfigPath(ui.dotaDir), Dota2ConfigText(), false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            Muted("Adds gamestate_integration_lumabridge.cfg to Dota 2's cfg folder (Valve's official way). Dota 2 "
                  "also needs -gamestateintegration in its launch options in Steam.");
        }
    } else if (key == "league") {
        Muted("%s", feeds.LeagueSeen() ? "League of Legends answered this session."
                                       : "Nothing to set up: it works as soon as you're in a match (Riot's own Live "
                                         "Client Data API on this PC).");
    } else if (key == "forza") {
        Muted("In the game: Settings > HUD and Gameplay (Forza Motorsport: Gameplay & HUD) > Data Out: On, Data Out "
              "IP Address: 127.0.0.1, Data Out IP Port: the port below.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Port");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * S());
        int port = ctl.prefs().forzaPort;
        if (ImGui::InputInt("##forzaport", &port, 0, 0) && port > 1024 && port < 65536) {
            ctl.prefs().forzaPort = port;
            ctl.Changed();
            ctl.RefreshFeedSettings();
        }
        ImGui::SameLine();
        Muted("%s", feeds.ForzaPortBusy() ? "Taken by another program (a telemetry app?) - pick another port here and in "
                                            "the game."
                    : feeds.ForzaSeen() ? "Receiving while you drive."
                                        : "Works while you drive, once Data Out is on.");
    } else {
        Muted("%s", feeds.WarThunderSeen() ? "Seen War Thunder's status page this session."
                                           : "Nothing to set up: it works as soon as you're in a battle.");
    }
    const std::string msg = ui.feedMessageId == key ? ui.feedMessage : in.LastId() == key ? in.LastMessage() : std::string();
    if (!msg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, V4(in.Busy() ? kAmber : msg.rfind("Done", 0) == 0 ? kGreen : kRed));
        ImGui::TextWrapped("%s", msg.c_str());
        ImGui::PopStyleColor();
    }
}

void OpenGame(UiState& ui, const std::string& name, const char* profileKey) {
    ui.gameDetail = name;
    ui.gameDetailProfile = profileKey ? profileKey : "";
    ui.feedCheckAt = 0;
}

// One game's page: its status, built-in lighting setup, and what it shows without lighting.
void GameDetailPage(Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    const InstalledGame* g = nullptr;
    for (const auto& x : ctl.library())
        if (ToUtf8(x.name) == ui.gameDetail) g = &x;
    const Controller::GameStatus* running = g ? RunningAs(ctl, *g) : nullptr;
    const games::GameProfile* profile =
        !ui.gameDetailProfile.empty() ? games::ProfileByKey(ui.gameDetailProfile.c_str()) : ProfileFor(g, running, ui.gameDetail);

    if (ImGui::Button("<  All games")) {
        ui.gameDetail.clear();
        return;
    }
    ImGui::Dummy(ImVec2(0, 2 * S()));

    BeginCard("game-head");
    IconItem(Icon::Game, 26 * S(), Hex(kAccent));
    ImGui::SameLine(0, 12 * S());
    ImGui::BeginGroup();
    ImGui::PushFont(f.title);
    ImGui::TextUnformatted(ui.gameDetail.c_str());
    ImGui::PopFont();
    if (g) {
        Pill(ToUtf8(g->store).c_str(), kMuted);
        ImGui::SameLine();
    }
    if (running) {
        Pill("Running", kAccent);
        ImGui::SameLine();
    }
    if (g) {
        const LightingStatus st = GameLighting(ctl, *g, running, profile);
        Pill(st.text, st.pill ? st.color : kMuted);
        if (!st.tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.tip.c_str());
    } else if (profile && profile->kind == games::ProfileKind::BuiltIn) {
        Pill("Built in", kGreen);
        ImGui::SameLine();
        Pill("Not found in your libraries", kMuted);
    }
    if (g) Muted("%s", ToUtf8(g->exePath.empty() ? g->dir : g->exePath).c_str());
    ImGui::EndGroup();
    EndCard();

    if (profile && profile->kind == games::ProfileKind::BuiltIn) {
        RefreshFeeds(ctl, ui);
        BeginCard("game-feed");
        IconItem(Icon::Lighting, 18 * S(), Hex(kAccent));
        ImGui::SameLine(0, 10 * S());
        ImGui::PushFont(f.bold);
        ImGui::TextUnformatted("Built-in lighting");
        ImGui::PopFont();
        ImGui::SameLine(0, 12 * S());
        FeedPill(ctl, ui, profile->key);
        ImGui::Dummy(ImVec2(0, 2 * S()));
        Muted("%s. %s", profile->how, profile->note);
        Muted("Official data the game publishes on your PC. Nothing is added to the game itself, so anti-cheat "
              "isn't involved.");
        ImGui::Dummy(ImVec2(0, 2 * S()));
        FeedSetup(ctl, in, ui, profile->key);
        EndCard();
    } else if (profile && profile->kind != games::ProfileKind::NotAGame) {
        BeginCard("game-about");
        CardTitle(f, "Lighting support", Icon::Info);
        Muted("%s. %s", profile->how, profile->note);
        EndCard();
    }

    if (!profile || profile->kind != games::ProfileKind::NotAGame) {
        BeginCard("game-mode");
        CardTitle(f, "When it isn't sending lighting", Icon::Palette);
        const std::string key = games::Normalize(ui.gameDetail);
        auto& modes = ctl.prefs().gameModes;
        auto it = modes.find(key);
        int mode = it == modes.end() ? 0 : static_cast<int>(it->second);
        if (Segmented("mode", &mode, kGameModeNames, 4, ImGui::GetContentRegionAvail().x)) {
            if (mode == 0) modes.erase(key);
            else modes[key] = static_cast<GameMode>(mode);
            if (mode == static_cast<int>(GameMode::Color) && !ctl.prefs().gameColors.count(key))
                ctl.prefs().gameColors[key] = ctl.prefs().manualColor;
            ctl.Changed();
        }
        static const char* kHelp[] = {
            "Follows \"Games without dynamic lighting show the screen's colors\" on the Lighting page.",
            "The lights run the screen's colors around the fans while this game runs (LumaBridge watches the "
            "screen image, never the game).",
            "Your choice for \"When no game is running\" (Lighting page) stays on while this game runs.",
            "The lights show this game's own color while it runs.",
        };
        if (running && ctl.screenColorsActive() && !ctl.screenProblem().empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, V4(kAmber));
            ImGui::TextWrapped("Screen colors can't read the screen: %s", ctl.screenProblem().c_str());
            ImGui::PopStyleColor();
        }
        if (mode == 0 && profile && profile->blocked)
            Muted("The game lights your Logitech gear itself, through G HUB (LIGHTSYNC). EA's anti-cheat keeps that "
                  "lighting away from LumaBridge, so your other devices show your choice for \"When no game is "
                  "running\" - or pick Screen colors here.");
        else
            Muted("%s", kHelp[mode]);
        if (mode == static_cast<int>(GameMode::Color)) {
            ImGui::Dummy(ImVec2(0, 4 * S()));
            Rgb& c = ctl.prefs().gameColors[key];
            if (ColorEditor(ctl, ui, &c, "gamecolor")) ctl.Changed();
        }
        Muted("Dynamic lighting from the game itself always comes first.");
        EndCard();
    }

    if (g && g->manual) {
        BeginCard("game-remove");
        CardTitle(f, "Added by you", Icon::Info);
        Muted("You added this game to the list yourself.");
        if (ImGui::Button("Remove from the list")) {
            const std::wstring exe = g->exePath;
            ui.gameDetail.clear();
            ctl.RemoveManualGame(exe);
        }
        EndCard();
    }
}

void GamesListPage(HWND hwnd, Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    if (!ui.gameDetail.empty()) {
        GameDetailPage(ctl, in, ui, f);
        return;
    }
    const auto& lib = ctl.library();

    // Built-in game lighting: the games LumaBridge lights through their official data.
    RefreshFeeds(ctl, ui);
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted("Built-in game lighting");
    ImGui::PopFont();
    Muted("These games light up through their own official data, no vendor software needed. Click one to set it up.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    static const char* kBuiltIn[] = {"cs2", "rocketleague", "warthunder", "dota2", "league", "forza"};
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = avail > 700 * S() ? 3 : 1;
    if (ImGui::BeginTable("builtin", cols, ImGuiTableFlags_SizingStretchSame)) {
        for (const char* key : kBuiltIn) {
            const games::GameProfile* p = games::ProfileByKey(key);
            ImGui::TableNextColumn();
            ImGui::PushID(key);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = 80 * S();
            const bool clicked = ImGui::InvisibleButton("tile", ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), Hex(hovered ? kCardHover : kCard), 14 * S());
            dl->AddRect(a, ImVec2(a.x + w, a.y + h), hovered ? Hex(kAccent, 180) : Hex(kBorder, 180), 14 * S());
            ImGui::SetCursorScreenPos(ImVec2(a.x + 16 * S(), a.y + 14 * S()));
            ImGui::BeginGroup();
            IconItem(Icon::Game, 20 * S(), Hex(kAccent));
            ImGui::SameLine(0, 10 * S());
            ImGui::PushFont(f.bold);
            ImGui::TextUnformatted(p->title);
            ImGui::PopFont();
            FeedPill(ctl, ui, key);
            ImGui::EndGroup();
            const char* more = "Customize  >";
            const ImVec2 ms = ImGui::CalcTextSize(more);
            dl->AddText(ImVec2(a.x + w - ms.x - 16 * S(), a.y + h - ms.y - 14 * S()), hovered ? Hex(kAccentHover) : Hex(kMuted), more);
            ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
            ImGui::Dummy(ImVec2(w, 4 * S()));
            if (clicked) OpenGame(ui, p->title, key);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, 8 * S()));

    BeginCard("library-head");
    CardTitle(f, "Games on this PC", Icon::Game);
    Muted("Found in Steam, Epic, EA, Ubisoft, GOG, Xbox, Riot, Rockstar and the games Windows knows about. "
          "Click a game to customize it. Missing one? Add it, and LumaBridge will recognise it whenever it runs.");
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
        ImGui::TableSetupColumn("Dynamic lighting", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("Without game lighting", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24 * S());
        ImGui::TableHeadersRow();
        int id = 0;
        for (const Row& row : rows) {
            const InstalledGame& g = *row.g;
            ImGui::PushID(id++);
            ImGui::TableNextRow(0, 34 * S());
            ImGui::TableNextColumn();
            const std::string name = ToUtf8(g.name);
            // The whole row opens the game's page.
            ImGui::PushStyleColor(ImGuiCol_Header, V4(kAccent, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, V4(kAccent, 0.14f));
            const bool open = ImGui::Selectable("##row", false,
                                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                                ImVec2(0, 26 * S()));
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ToUtf8(g.exePath.empty() ? g.dir : g.exePath).c_str());
            ImGui::SameLine(0, 0);
            ImGui::AlignTextToFramePadding();
            if (row.running) ImGui::PushFont(f.bold);
            ImGui::TextUnformatted(name.c_str());
            if (row.running) ImGui::PopFont();
            if (row.running) {
                ImGui::SameLine();
                Pill("Running", kAccent);
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            Muted("%s", ToUtf8(g.store).c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const games::GameProfile* profile = ProfileFor(&g, row.running, name);
            const LightingStatus st = GameLighting(ctl, g, row.running, profile);
            if (st.pill) Pill(st.text, st.color);
            else Muted("%s", st.text);
            if (!st.tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.tip.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (!profile || profile->kind != games::ProfileKind::NotAGame) {
                const std::string key = games::Normalize(name);
                auto it = ctl.prefs().gameModes.find(key);
                const int mode = it == ctl.prefs().gameModes.end() ? 0 : static_cast<int>(it->second);
                if (mode == static_cast<int>(GameMode::Color)) {
                    auto c = ctl.prefs().gameColors.find(key);
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const float r = 6 * S(), y = p.y + ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset + ImGui::GetTextLineHeight() / 2;
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, y), r,
                                                                Col(c == ctl.prefs().gameColors.end() ? Rgb{} : c->second));
                    ImGui::Dummy(ImVec2(r * 2, 1));
                    ImGui::SameLine();
                }
                if (mode) ImGui::TextUnformatted(kGameModeNames[mode]);
                else Muted("Default");
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            Muted(">");
            ImGui::PopID();
            if (open) OpenGame(ui, name, nullptr);
        }
        ImGui::EndTable();
    }
    EndCard();
}

// Devices with Windows' lighting standard built in: nothing to install.
void LampArrayCard(Controller& ctl, const Fonts& f) {
    BeginCard("lamparray");
    IconItem(Icon::Plug, 18 * S(), Hex(kAccent));
    ImGui::SameLine(0, 10 * S());
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted("Windows Dynamic Lighting devices");
    ImGui::PopFont();
    ImGui::SameLine();
    const auto lit = LampArrayLit(ctl);
    char pill[48];
    snprintf(pill, sizeof pill, "%d device%s", static_cast<int>(lit.size()), lit.size() == 1 ? "" : "s");
    if (!ctl.prefs().lampArray) Pill("Off", kMuted);
    else if (!lit.empty()) Pill(pill, kGreen);
    else Pill("None found", kMuted);
    Muted("Built in: keyboards, mice, headsets, cases and light strips of any brand with Windows' lighting standard "
          "(HID LampArray, the one Windows 11's Dynamic Lighting uses) in their firmware follow LumaBridge and your "
          "games, lamp by lamp, with no software from their maker. New devices appear by themselves.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    bool on = ctl.prefs().lampArray;
    if (Toggle("Enabled", &on)) ctl.SetLampArrayEnabled(on);
    EndCard();
}

// OpenRGB: every device it supports, through its SDK server (optional; nothing to install
// in LumaBridge).
void OpenRgbCard(Controller& ctl, const Fonts& f) {
    BeginCard("openrgb");
    IconItem(Icon::Plug, 18 * S(), Hex(kAccent));
    ImGui::SameLine(0, 10 * S());
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted("OpenRGB");
    ImGui::PopFont();
    ImGui::SameLine();
    const auto st = ctl.openRgb().state();
    const auto devs = ctl.openRgb().devices();
    char pill[48];
    snprintf(pill, sizeof pill, "Connected, %d device%s", static_cast<int>(devs.size()), devs.size() == 1 ? "" : "s");
    if (!ctl.prefs().openRgb) Pill("Off", kMuted);
    else if (st == OpenRgbOutput::State::Connected) Pill(pill, kGreen);
    else Pill("OpenRGB isn't running", kAmber);
    Muted("Only if you already use OpenRGB: LumaBridge then also lights what OpenRGB supports, while OpenRGB runs "
          "with its SDK server on (its SDK Server tab > Start Server). LumaBridge doesn't need it. Devices LumaBridge "
          "lights itself are left to LumaBridge.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    bool on = ctl.prefs().openRgb;
    if (Toggle("Enabled", &on)) ctl.SetOpenRgbEnabled(on);
    ImGui::SameLine(0, 24 * S());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Port");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * S());
    int port = ctl.prefs().openRgbPort;
    if (ImGui::InputInt("##orgbport", &port, 0, 0) && port > 0 && port < 65536) {
        ctl.prefs().openRgbPort = port;
        ctl.Changed();
        if (ctl.prefs().openRgb) {  // reconnect on the new port
            ctl.SetOpenRgbEnabled(false);
            ctl.SetOpenRgbEnabled(true);
        }
    }
    EndCard();
}

void IntegrationsPage(HWND hwnd, Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    const auto& gs = ctl.gameSense();
    in.SetOwner(hwnd);
    EnsureIntegrations(ctl, in, ui);

    Muted("LumaBridge answers games as if the vendor's software and devices were installed, then sends "
          "the colors to your Aura devices.");
    ImGui::Dummy(ImVec2(0, 4 * S()));

    for (const auto& it : in.list()) {
        BeginCard(it.id.c_str());
        IconItem(it.id == "handback" ? Icon::Lighting : it.id == "helper" ? Icon::Gear : Icon::Plug, 18 * S(), Hex(kAccent));
        ImGui::SameLine(0, 10 * S());
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
            if (Toggle("Enabled", &on)) {
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
        } else if ((it.id == "handback" || it.id == "helper") && it.state != IntegrationState::NotInstalled) {
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
            if (PrimaryButton(it.id == "logitech" || it.id == "handback" || it.id == "helper" ? "Set up" : "Install"))
                in.Install(it.id);
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

    LampArrayCard(ctl, f);
    OpenRgbCard(ctl, f);

    Muted("Counter-Strike 2, Rocket League and War Thunder light up through their own official data "
          "instead: set them up on the Games List page.");
}

void SettingsPage(Controller& ctl, UiState& ui, const Fonts& f) {
    Config& cfg = ctl.config();

    BeginCard("calibration");
    CardTitle(f, "Color calibration", Icon::Palette);
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
    CardTitle(f, "Dynamic lighting", Icon::Lighting);
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
    CardTitle(f, "Startup", Icon::Gear);
    if (!ui.autostartLoaded) {
        ui.autostart = IsAutostartEnabled();
        ui.autostartLoaded = true;
    }
    if (Toggle("Start LumaBridge with Windows", &ui.autostart)) SetAutostart(ui.autostart);
    if (Toggle("Start minimized to the tray", &ctl.prefs().startMinimized)) ctl.Changed();
    EndCard();

    BeginCard("guide");
    CardTitle(f, "Setup guide", Icon::Plug);
    Muted("Asks which RGB hardware and lighting software you have, and sets up LumaBridge's connections to match.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (ImGui::Button("Run the setup guide again")) {
        ui.setupStep = 0;
        ui.setupDetected = false;
    }
    EndCard();

    BeginCard("about");
    CardTitle(f, "About", Icon::Info);
    ImGui::Text("LumaBridge %s", kVersionText);
    Muted("Game lighting for ASUS Aura, without Armoury Crate in the way.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (ImGui::Button("Releases on GitHub"))
        ShellExecuteW(nullptr, L"open", L"https://github.com/EmilB04/LumaBridge/releases", nullptr, nullptr, SW_SHOWNORMAL);
    EndCard();

    BeginCard("trouble");
    CardTitle(f, "Troubleshooting", Icon::Info);
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

    {
        // Logo: the app's icon (else a slowly turning ring of hues).
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float r = 13 * S();
        const ImVec2 c(p.x + r, p.y + r + 2 * S());
        if (f.logo) {
            ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(f.logo), ImVec2(c.x - r, c.y - r),
                                                 ImVec2(c.x + r, c.y + r));
        } else {
            const double t = ImGui::GetTime() * 0.08;
            for (int i = 0; i < 24; ++i) {
                const float a0 = 6.2831853f * i / 24, a1 = 6.2831853f * (i + 1) / 24;
                const Rgb col = FromHue((static_cast<double>(i) / 24 + t) * 360.0);
                ImGui::GetWindowDrawList()->PathArcTo(c, r - 2.5f * S(), a0, a1 + 0.05f, 4);
                ImGui::GetWindowDrawList()->PathStroke(Col(col), 0, 4 * S());
            }
        }
        ImGui::Dummy(ImVec2(r * 2, r * 2 + 4 * S()));
        ImGui::SameLine(0, 10 * S());
        ImGui::BeginGroup();
        ImGui::PushFont(f.title);
        ImGui::TextUnformatted("LumaBridge");
        ImGui::PopFont();
        ImGui::PushFont(f.caption);
        Muted("Game lighting for Aura");
        ImGui::PopFont();
        ImGui::EndGroup();
    }
    ImGui::Dummy(ImVec2(0, 18 * S()));

    const struct {
        Page page;
        const char* label;
        Icon icon;
    } items[] = {{Page::Dashboard, "Dashboard", Icon::Grid},     {Page::Lighting, "Lighting", Icon::Lighting},
                 {Page::GamesList, "Games List", Icon::Game},     {Page::Devices, "Devices", Icon::Leds},
                 {Page::Integrations, "Integrations", Icon::Plug}, {Page::Settings, "Settings", Icon::Gear}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& it : items) {
        const bool sel = ui.page == it.page;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 40 * S();
        ImGui::PushID(it.label);
        if (ImGui::InvisibleButton("nav", ImVec2(w, h))) ui.page = it.page;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (sel) {
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Hex(kAccent, 42), 10 * S());
            dl->AddRectFilled(ImVec2(p.x, p.y + 10 * S()), ImVec2(p.x + 3 * S(), p.y + h - 10 * S()), Hex(kAccentHover), 2 * S());
        } else if (hovered) {
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Hex(kCardHover), 10 * S());
        }
        DrawIcon(it.icon, ImVec2(p.x + 22 * S(), p.y + h / 2), 18 * S(), sel ? Hex(kAccentHover) : hovered ? Hex(kText) : Hex(kMuted));
        ImGui::PushFont(sel ? f.bold : f.regular);
        dl->AddText(ImVec2(p.x + 44 * S(), p.y + (h - ImGui::GetFontSize()) / 2), sel || hovered ? Hex(kText) : Hex(kMuted), it.label);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2 * S()));
    }

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
    s.FrameRounding = 10;
    s.GrabRounding = 10;
    s.GrabMinSize = 14;
    s.ChildRounding = 14;
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

// The loading screen when the window first opens: the logo growing in with a ring of hues
// turning around it, then fading into the app. Covers the window (and takes the clicks)
// while it shows.
void Splash(UiState& ui, const Fonts& f) {
    constexpr double kShow = 1.3, kFade = 0.4;
    const double now = ImGui::GetTime();
    if (ui.splashStart < 0) ui.splashStart = now;
    const double t = now - ui.splashStart;
    if (t >= kShow + kFade) return;
    const float alpha = t < kShow ? 1.f : static_cast<float>(1.0 - (t - kShow) / kFade);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(0);
    ImGui::Begin("##splash", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFocus();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto a = [&](unsigned rgb, float k = 1.f) { return Hex(rgb, static_cast<unsigned>(255 * alpha * k)); };
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), a(kBg));
    const ImVec2 c(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2 - 30 * S());
    // The logo grows in (ease out over the first half second).
    const float grow = static_cast<float>(std::min(1.0, t / 0.5));
    const float ease = 1 - (1 - grow) * (1 - grow) * (1 - grow);
    const float r = (38 + 10 * ease) * S();
    if (f.logo) {
        dl->AddImage(static_cast<ImTextureID>(f.logo), ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(0, 0),
                     ImVec2(1, 1), IM_COL32(255, 255, 255, static_cast<int>(255 * alpha * ease)));
    } else {
        dl->AddCircleFilled(c, r, a(kAccent, ease), 64);
    }
    // A comet of hues turning around it.
    const float ring = r + 14 * S();
    const float head = static_cast<float>(t * 4.2);
    for (int i = 0; i < 28; ++i) {
        const float k = static_cast<float>(i) / 28;
        const float a0 = head - k * 3.6f, a1 = a0 + 0.14f;
        const Rgb col = FromHue(std::fmod(t * 90.0 + k * 140.0, 360.0));
        dl->PathArcTo(c, ring, a0, a1, 4);
        dl->PathStroke(Col(col, static_cast<int>(255 * alpha * (1 - k))), 0, 3.5f * S() * (1 - k * 0.6f));
    }
    // The name under it.
    ImGui::PushFont(f.title);
    const char* name = "LumaBridge";
    const ImVec2 ns = ImGui::CalcTextSize(name);
    dl->AddText(ImVec2(c.x - ns.x / 2, c.y + ring + 18 * S()), a(kText), name);
    ImGui::PopFont();
    ImGui::PushFont(f.caption);
    const char* sub = "Starting...";
    const ImVec2 ss = ImGui::CalcTextSize(sub);
    dl->AddText(ImVec2(c.x - ss.x / 2, c.y + ring + 18 * S() + ns.y + 6 * S()), a(kMuted), sub);
    ImGui::PopFont();
    ImGui::End();
}

// ---- Setup guide -----------------------------------------------------------------------
// Asks what hardware and lighting software the PC has (detection ticks what it finds), then
// sets up LumaBridge's connections: everything on, except what the answers rule out.

using setup::App;
using setup::Brand;
using setup::Conn;
using SetupState = UiState::SetupState;

const char* kSetupSteps[] = {"Welcome", "Hardware", "Software", "Devices", "Connections", "Set up", "Your look"};
enum SetupStep { kStepWelcome, kStepHardware, kStepSoftware, kStepDevices, kStepConnections, kStepRun, kStepLook };

// The guide's "Your devices" step: what LumaBridge found, and the fans on the ARGB header,
// which it can't count itself.
void SetupDevicesStep(Controller& ctl, UiState& ui, const Fonts& f) {
    const auto& pres = ctl.presence();
    const sensors::SystemSnapshot snap = ctl.monitor().Snapshot();
    const SetupHardware hw = DetectSetup(snap.smbios);
    auto line = [&](Icon icon, bool ok, const std::string& name, const std::string& what) {
        IconItem(icon, 18 * S(), ok ? Hex(kAccent) : Hex(kMuted));
        ImGui::SameLine(0, 10 * S());
        ImGui::PushFont(f.bold);
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        Muted("%s", what.c_str());
    };

    BeginCard("found");
    CardTitle(f, "Found on this PC", Icon::Leds);
    int auraCount = 0;
    bool header = false;
    for (const auto& d : ctl.devices()) {
        Icon icon;
        line(Icon::Board, true, DeviceLabel(d, snap, ctl.config().argbFans, &icon), std::to_string(d.lightCount) + " LEDs");
        ++auraCount;
        header |= d.type == 0x00011000;
    }
    if (!auraCount)
        line(Icon::Board, false, "No ASUS Aura controller found",
             ui.setupAnswers.has(Brand::Asus) ? "- try Devices > Rescan devices later; the log says why" : "");
    if (!pres.scanned) {
        Muted("Looking for Logitech devices and keyboards...");
    } else {
        for (const auto& d : pres.logitech)
            line(Icon::Mouse, d.rgb, "Logitech " + (d.name.empty() ? std::string("device") : d.name),
                 std::string(hidpp::DeviceTypeName(d.type)) + (d.rgb ? ", RGB lighting" : " - no RGB lighting, left out"));
        if (pres.azoth) line(Icon::Keyboard, true, "ASUS ROG Azoth", "Keyboard, every key its own color");
    }
    for (const auto& d : ctl.lampArray().devices())
        line(Icon::Leds, ctl.LampArrayOn(d), d.name,
             std::string(lamparray::KindName(d.kind)) +
                 (!d.problem.empty()           ? " - " + d.problem
                  : ctl.LampArrayOn(d)         ? ", " + std::to_string(d.lamps) + " lamps, Windows lighting standard"
                                               : " - lit by LumaBridge another way"));
    for (const auto& d : ctl.openRgb().devices())
        line(Icon::Leds, ctl.OpenRgbOn(d), d.name,
             std::string(openrgb::TypeName(d.type)) + (ctl.OpenRgbOn(d) ? ", through OpenRGB" : " - lit by LumaBridge itself"));
    if (hw.ram != RamStyle::Generic)
        line(Icon::Memory, true, hw.ramName.empty() ? "RGB memory" : hw.ramName,
             "RGB memory - confirmed once Hardware access is set up");
    else if (!hw.ramName.empty() && catalog::RgbMemory(snap.smbios.memory.empty() ? "" : snap.smbios.memory[0].manufacturer,
                                                         snap.smbios.memory.empty() ? "" : snap.smbios.memory[0].part)
                                            .empty())
        line(Icon::Memory, false, hw.ramName, "- no RGB memory LumaBridge can light");
    if (const auto also = AlsoFound(ctl, snap); !also.empty()) {
        const bool connected = ctl.openRgb().state() == OpenRgbOutput::State::Connected;
        ImGui::Dummy(ImVec2(0, 4 * S()));
        ImGui::PushStyleColor(ImGuiCol_Text, V4(connected ? kMuted : kAmber));
        ImGui::TextWrapped("Also found: %s. %s", Join(also).c_str(),
                           connected ? "OpenRGB lists what it supports of these above."
                                     : "LumaBridge can't light these directly yet: they don't use Windows' lighting "
                                       "standard. Their own app lights them (or OpenRGB, if you already use it).");
        ImGui::PopStyleColor();
    }
    EndCard();

    if (header || (!auraCount && ui.setupAnswers.has(Brand::Asus))) {
        BeginCard("fans");
        CardTitle(f, "Fans on the ARGB header", Icon::Fan);
        Muted("LumaBridge can't count fans itself. How many are on the motherboard's ARGB header (fans chained on "
              "a hub count too), and how many LEDs does each have? The box or product page says.");
        ImGui::Dummy(ImVec2(0, 4 * S()));
        fx::FanLayout& l = ctl.config().argbFans;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Fans");
        ImGui::SameLine(120 * S());
        for (int n = 1; n <= 8; ++n) {
            ImGui::PushID(n);
            char b[4];
            snprintf(b, sizeof b, "%d", n);
            if (n == l.Fans() ? PrimaryButton(b, ImVec2(34 * S(), 0)) : ImGui::Button(b, ImVec2(34 * S(), 0))) {
                l.fans = n;
                ctl.Changed();
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("LEDs per fan");
        ImGui::SameLine(120 * S());
        for (int n : {8, 12, 16, 18, 20, 24}) {
            ImGui::PushID(100 + n);
            char b[4];
            snprintf(b, sizeof b, "%d", n);
            if (n == l.LedsPerFan() ? PrimaryButton(b, ImVec2(34 * S(), 0)) : ImGui::Button(b, ImVec2(34 * S(), 0))) {
                l.ledsPerFan = n;
                ctl.Changed();
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(130 * S());
        if (ImGui::InputInt("##leds", &l.ledsPerFan, 1, 4)) {
            l.ledsPerFan = l.LedsPerFan();
            ctl.Changed();
        }
        ImGui::Dummy(ImVec2(0, 4 * S()));
        bool test = ctl.fanTest();
        if (Toggle("Show test pattern", &test)) ctl.SetFanTest(test);
        ImGui::SameLine();
        Muted("Each fan one solid color with a single white LED. A color spilling onto the next fan: change LEDs "
              "per fan.");
        if (ctl.fanTest()) LightsPreview(ctl.output(), l, 0);
        EndCard();
    }
    if (!snap.smbios.memory.empty()) RamSlotsCard(ctl, f);
}

void SetupStepper(int step, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const int n = static_cast<int>(std::size(kSetupSteps));
    const float seg = width / n, r = 11 * S(), y = p.y + r;
    for (int i = 0; i < n; ++i) {
        const ImVec2 c(p.x + seg * i + seg / 2, y);
        if (i + 1 < n)
            dl->AddLine(ImVec2(c.x + r + 4 * S(), y), ImVec2(c.x + seg - r - 4 * S(), y),
                        Hex(i < step ? kAccent : kBorder), 2 * S());
        if (i < step) {
            dl->AddCircleFilled(c, r, Hex(kAccent), 24);
            dl->AddLine(ImVec2(c.x - 4.5f * S(), c.y), ImVec2(c.x - 1 * S(), c.y + 3.5f * S()), Hex(0xFFFFFF), 2 * S());
            dl->AddLine(ImVec2(c.x - 1 * S(), c.y + 3.5f * S()), ImVec2(c.x + 5 * S(), c.y - 3.5f * S()), Hex(0xFFFFFF),
                        2 * S());
        } else {
            dl->AddCircleFilled(c, r, Hex(i == step ? kAccent : kTrack), 24);
            dl->AddCircle(c, r, Hex(i == step ? kAccentHover : kBorder), 24, 1.5f * S());
            char num[4];
            snprintf(num, sizeof num, "%d", i + 1);
            const ImVec2 ns = ImGui::CalcTextSize(num);
            dl->AddText(ImVec2(c.x - ns.x / 2, c.y - ns.y / 2), Hex(i == step ? 0xFFFFFF : kMuted), num);
        }
        const ImVec2 ls = ImGui::CalcTextSize(kSetupSteps[i]);
        dl->AddText(ImVec2(c.x - ls.x / 2, y + r + 6 * S()), Hex(i == step ? kText : kMuted), kSetupSteps[i]);
    }
    ImGui::Dummy(ImVec2(width, 2 * r + 6 * S() + ImGui::GetTextLineHeight() + 6 * S()));
}

// A tile that ticks on and off: title, a line under it, "Found on this PC" when detected.
bool ChoiceTile(const Fonts& f, const char* id, const char* title, const char* sub, bool found, bool* on, float w) {
    ImGui::PushID(id);
    const float h = 66 * S();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("tile", ImVec2(w, h));
    if (clicked) *on = !*on;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(p.x + w, p.y + h);
    dl->AddRectFilled(p, b, Hex(hovered ? kCardHover : kCard), 10 * S());
    dl->AddRect(p, b, Hex(*on ? kAccent : kBorder), 10 * S(), 0, (*on ? 2.f : 1.f) * S());
    // The tick, right.
    const ImVec2 c(b.x - 24 * S(), p.y + h / 2);
    const float r = 10 * S();
    if (*on) {
        dl->AddCircleFilled(c, r, Hex(kAccent), 24);
        dl->AddLine(ImVec2(c.x - 4.5f * S(), c.y), ImVec2(c.x - 1 * S(), c.y + 3.5f * S()), Hex(0xFFFFFF), 2 * S());
        dl->AddLine(ImVec2(c.x - 1 * S(), c.y + 3.5f * S()), ImVec2(c.x + 5 * S(), c.y - 3.5f * S()), Hex(0xFFFFFF),
                    2 * S());
    } else {
        dl->AddCircle(c, r, Hex(kBorder), 24, 1.5f * S());
    }
    const float x = p.x + 16 * S();
    dl->AddText(f.bold, f.bold->FontSize, ImVec2(x, p.y + 12 * S()), Hex(kText), title);
    if (found) {
        const float tw = f.bold->CalcTextSizeA(f.bold->FontSize, FLT_MAX, 0, title).x;
        const char* tag = "Found on this PC";
        const float cs = f.caption->FontSize;
        const ImVec2 ts = f.caption->CalcTextSizeA(cs, FLT_MAX, 0, tag);
        const ImVec2 ta(x + tw + 10 * S(), p.y + 12 * S() + (f.bold->FontSize - ts.y) / 2 - 2 * S());
        dl->AddRectFilled(ta, ImVec2(ta.x + ts.x + 12 * S(), ta.y + ts.y + 4 * S()), Hex(kGreen, 40), 8 * S());
        dl->AddText(f.caption, cs, ImVec2(ta.x + 6 * S(), ta.y + 2 * S()), Hex(kGreen), tag);
    }
    dl->AddText(f.caption, f.caption->FontSize, ImVec2(x, p.y + 16 * S() + f.bold->FontSize), Hex(kMuted), sub);
    ImGui::PopID();
    return clicked;
}

// OpenRGB installed or running (checked at most every few seconds: it lists processes).
bool OpenRgbOnPc() {
    static double checkedAt = -100;
    static bool present = false;
    if (ImGui::GetTime() - checkedAt > 5) {
        present = OpenRgbPresent();
        checkedAt = ImGui::GetTime();
    }
    return present;
}

const Integration* FindIntegration(const Integrations& in, const char* id) {
    for (const auto& it : in.list())
        if (it.id == id) return &it;
    return nullptr;
}

// The connections ticked to start with, from the answers (and what's already set up).
void SetupDefaults(Controller& ctl, const Integrations& in, UiState& ui) {
    for (int i = 0; i < setup::kConns; ++i) {
        const Conn c = static_cast<Conn>(i);
        bool on = setup::DefaultOn(c, ui.setupAnswers);
        if (c == Conn::OpenRgb) on = ctl.prefs().openRgb || OpenRgbOnPc();
        if (const Integration* it = FindIntegration(in, setup::Info(c).integration)) {
            if (it->state == IntegrationState::Active) on = true;
            if (it->state == IntegrationState::Conflict) on = false;  // a vendor runtime would be replaced
        }
        ui.setupConns[i] = on;
    }
}

// Applies the switches, then installs the ticked connections one after another.
void SetupRun(Controller& ctl, Integrations& in, UiState& ui) {
    if (ui.setupNext < 0) {
        for (int i = 0; i < setup::kConns; ++i) {
            const Conn c = static_cast<Conn>(i);
            const bool on = ui.setupConns[i];
            if (setup::Info(c).install) {
                ui.setupState[i] = on ? SetupState::Pending : SetupState::Skipped;
                ui.setupResult[i] = on ? "Waiting" : "Left as it is";
                continue;
            }
            switch (c) {
            case Conn::GameSense:
                if (ctl.gameSense().IsRunning() != on) ctl.SetGameSenseEnabled(on);
                break;
            case Conn::LogitechDevices:
                if (ctl.prefs().logitechDevices != on) ctl.SetLogitechEnabled(on);
                break;
            case Conn::Azoth: ctl.SetAzothEnabled(on); break;
            case Conn::RamLighting: ctl.SetRamEnabled(on); break;
            case Conn::OpenRgb:
                if (ctl.prefs().openRgb != on) ctl.SetOpenRgbEnabled(on);
                break;
            case Conn::LampArray:
                if (ctl.prefs().lampArray != on) ctl.SetLampArrayEnabled(on);
                break;
            default: break;
            }
            ui.setupState[i] = on ? SetupState::Done : SetupState::Skipped;
            ui.setupResult[i] = on ? "On" : "Off";
        }
        ui.setupNext = 0;
    }
    while (ui.setupNext < setup::kConns) {
        const int i = ui.setupNext;
        const setup::ConnInfo& ci = setup::Info(static_cast<Conn>(i));
        if (ui.setupState[i] != SetupState::Pending && ui.setupState[i] != SetupState::Running) {
            ++ui.setupNext;
            continue;
        }
        if (ui.setupState[i] == SetupState::Pending) {
            const Integration* it = FindIntegration(in, ci.integration);
            if (it && it->state == IntegrationState::Active) {
                ui.setupState[i] = SetupState::Done;
                ui.setupResult[i] = "Already set up";
                ++ui.setupNext;
                continue;
            }
            if (in.Busy()) return;
            in.Install(ci.integration, L"", it && it->state == IntegrationState::Conflict);
            ui.setupState[i] = SetupState::Running;
            ui.setupResult[i] = ci.admin ? "Approve the Windows administrator prompt..." : "Setting up...";
            return;
        }
        if (in.Busy()) return;  // still running
        // Trust what's on the PC afterwards, not only the script's exit code.
        const std::string msg = in.LastMessage();
        ui.integrationsLoaded = false;
        EnsureIntegrations(ctl, in, ui);
        const Integration* it = FindIntegration(in, ci.integration);
        const bool ok = msg.rfind("Done", 0) == 0 && it && it->state == IntegrationState::Active;
        ui.setupState[i] = ok ? SetupState::Done : SetupState::Failed;
        ui.setupResult[i] = ok ? "Set up"
                            : msg.rfind("Done", 0) == 0
                                ? "Finished, but it isn't set up - see setup.log in the log folder (Settings)"
                                : msg;
        ++ui.setupNext;
    }
}

void SetupFinish(Controller& ctl, UiState& ui) {
    // The look step showed the look on every device in Manual; now what the user chose: games
    // take over (their look while no game runs) or always their look.
    if (ui.setupModeBefore >= 0) {
        ctl.prefs().mode = ui.setupAuto ? Mode::Auto : Mode::Manual;
        if (ui.setupAuto) ctl.prefs().idle = IdleBehavior::ManualColor;
        ui.setupModeBefore = -1;
    }
    ctl.prefs().setupDone = true;
    ctl.Changed();
    ui.setupStep = -1;
    ui.page = Page::Dashboard;
    ui.integrationsLoaded = false;
}

void SetupGuide(Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    EnsureIntegrations(ctl, in, ui);
    if (ui.setupStep != kStepDevices && ctl.fanTest()) ctl.SetFanTest(false);  // the test is on the devices step
    const float avail = ImGui::GetContentRegionAvail().x;
    const float colW = std::min(avail, 780 * S());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - colW) / 2);
    ImGui::BeginChild("guide", ImVec2(colW, 0));
    ImGui::Dummy(ImVec2(0, 10 * S()));
    SetupStepper(ui.setupStep, colW);
    ImGui::Dummy(ImVec2(0, 10 * S()));
    auto heading = [&](const char* title, const char* text) {
        ImGui::PushFont(f.title);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        Muted("%s", text);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8 * S()));
    };
    const float gap = 12 * S(), tileW = (colW - gap) / 2;
    auto footer = [&](bool back, const char* next) {
        ImGui::Dummy(ImVec2(0, 12 * S()));
        if (back && ImGui::Button("Back", ImVec2(110 * S(), 0))) --ui.setupStep;
        const float nw = std::max(150 * S(), ImGui::CalcTextSize(next).x + 40 * S());
        ImGui::SameLine(ImGui::GetContentRegionMax().x - nw);
        return PrimaryButton(next, ImVec2(nw, 0));
    };

    switch (ui.setupStep) {
    case kStepWelcome: {
        if (f.logo) {
            const float s = 72 * S();
            ImGui::Image(static_cast<ImTextureID>(f.logo), ImVec2(s, s));
        }
        heading("Welcome to LumaBridge",
                "A few questions to get your lighting going: which RGB hardware you have and which lighting "
                "software runs on this PC. LumaBridge looks for them itself and ticks what it finds - you only "
                "correct what's wrong. Then it sets up its connections, all on unless your answers rule one out.");
        ImGui::Dummy(ImVec2(0, 8 * S()));
        if (ImGui::Button("Skip - I'll set it up myself")) SetupFinish(ctl, ui);
        const float nw = 150 * S();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - nw);
        if (PrimaryButton("Get started", ImVec2(nw, 0))) {
            if (!ui.setupDetected) {
                ui.setupFound = DetectVendors(DetectSetup(ctl.monitor().Snapshot().smbios), !ctl.devices().empty());
                ui.setupAnswers = ui.setupFound;
                ui.setupDetected = true;
            }
            ui.setupStep = kStepHardware;
        }
        break;
    }
    case kStepHardware: {
        heading("What RGB hardware do you have?",
                "Ticked: found on this PC. Tick anything LumaBridge missed, untick what you don't have.");
        for (int i = 0; i < setup::kBrands; ++i) {
            if (i % 2) ImGui::SameLine(0, gap);
            const Brand b = static_cast<Brand>(i);
            bool on = ui.setupAnswers.has(b);
            if (ChoiceTile(f, setup::Info(b).name, setup::Info(b).name, setup::Info(b).what, ui.setupFound.has(b), &on,
                           tileW))
                ui.setupAnswers.set(b, on);
            if (i % 2) ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
        }
        if (footer(true, "Next")) ui.setupStep = kStepSoftware;
        break;
    }
    case kStepSoftware: {
        heading("Which lighting software do you use?",
                "LumaBridge works alongside it: it hands the lights back to Armoury Crate, passes game lighting on to "
                "SteelSeries GG, and doesn't replace Razer's or Alienware's own game lighting.");
        for (int i = 0; i < setup::kApps; ++i) {
            if (i % 2) ImGui::SameLine(0, gap);
            const App a = static_cast<App>(i);
            bool on = ui.setupAnswers.has(a);
            char sub[96];
            snprintf(sub, sizeof sub, "%s's lighting app", setup::Info(a).vendor);
            if (ChoiceTile(f, setup::Info(a).name, setup::Info(a).name, sub, ui.setupFound.has(a), &on, tileW))
                ui.setupAnswers.set(a, on);
            if (i % 2) ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
        }
        if (footer(true, "Next")) {
            ctl.RescanPresence();
            ui.setupStep = kStepDevices;
        }
        break;
    }
    case kStepDevices: {
        heading("Your devices",
                "What LumaBridge found. Only these show up in LumaBridge; plug something in later and it appears by "
                "itself (or Devices > Rescan devices).");
        SetupDevicesStep(ctl, ui, f);
        if (footer(true, "Next")) {
            SetupDefaults(ctl, in, ui);
            ui.setupStep = kStepConnections;
        }
        break;
    }
    case kStepConnections: {
        heading("LumaBridge's connections",
                "All on, except where your answers say otherwise. Change anything you like - everything can be "
                "changed later on the Devices and Integrations pages.");
        int admin = 0;
        for (int i = 0; i < setup::kConns; ++i) {
            const Conn c = static_cast<Conn>(i);
            const setup::ConnInfo& ci = setup::Info(c);
            if (i == 0 || i == static_cast<int>(Conn::LogitechDevices) || i == static_cast<int>(Conn::Handback)) {
                ImGui::Dummy(ImVec2(0, 4 * S()));
                ImGui::PushFont(f.bold);
                ImGui::TextUnformatted(i == 0 ? "Games" : i == static_cast<int>(Conn::Handback) ? "Armoury Crate"
                                                                                               : "Your devices");
                ImGui::PopFont();
            }
            const Integration* it = FindIntegration(in, ci.integration);
            const bool already = it && it->state == IntegrationState::Active;
            ImGui::PushID(i);
            BeginCard("conn");
            ImGui::BeginDisabled(already);
            bool on = ui.setupConns[i] || already;
            if (Toggle(ci.name, &on)) ui.setupConns[i] = on;
            ImGui::EndDisabled();
            if (already) {
                ImGui::SameLine();
                Pill("Already set up", kGreen);
            } else if (ci.admin && on) {
                ImGui::SameLine();
                Pill("Administrator approval", kAmber);
                ++admin;
            }
            Muted("%s", ci.why);
            if (!on && !already) {
                const char* why = it && it->state == IntegrationState::Conflict ? it->detail.c_str()
                                  : c == Conn::OpenRgb && OpenRgbOnPc()                ? ""
                                                                                          : setup::OffReason(c, ui.setupAnswers);
                if (*why) {
                    ImGui::PushStyleColor(ImGuiCol_Text, V4(kAmber));
                    ImGui::TextWrapped("%s", why);
                    ImGui::PopStyleColor();
                }
            }
            EndCard();
            ImGui::PopID();
        }
        Muted("Corsair iCUE games are added one game at a time, on the Integrations page.");
        if (admin)
            Muted("Windows asks for administrator approval %d time%s, once for each marked connection.", admin,
                  admin == 1 ? "" : "s");
        if (footer(true, "Set up")) {
            ui.setupNext = -1;
            ui.setupStep = kStepRun;
        }
        break;
    }
    case kStepLook: {
        // Every device shows the look right now: Manual while this step is open.
        if (ui.setupModeBefore < 0) {
            ui.setupModeBefore = static_cast<int>(ctl.prefs().mode);
            ctl.prefs().mode = Mode::Manual;
            ctl.Changed();
        }
        heading("Pick your look",
                "Click one: every device shows it right away - fans, board, memory, keyboard and mouse together. "
                "Try a few. Everything can be fine-tuned later on the Lighting page.");
        if (ctl.auraPaused()) {
            ImGui::PushStyleColor(ImGuiCol_Text, V4(kAmber));
            ImGui::TextWrapped("LumaBridge isn't controlling the lights right now.");
            ImGui::PopStyleColor();
            if (PrimaryButton("Resume lighting control")) ctl.ResumeAura();
            ImGui::Dummy(ImVec2(0, 6 * S()));
        }
        Look look = MainLook(ctl.prefs());
        bool picked = false;
        auto group = [&](const char* title, std::initializer_list<fx::Kind> kinds) {
            ImGui::PushFont(f.bold);
            ImGui::TextUnformatted(title);
            ImGui::PopFont();
            ImGui::PushID(title);
            picked |= PresetTilesOf(ctl, look, kinds);
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 6 * S()));
        };
        group("Gradients", {fx::Kind::Gradient});
        group("Rainbows", {fx::Kind::RainbowWave, fx::Kind::ColorCycle});
        group("Moving", {fx::Kind::Comet, fx::Kind::Twinkle});
        group("Calm", {fx::Kind::Breathing, fx::Kind::Static});
        if (picked) {
            SetMainLook(ctl.prefs(), look);
            for (auto& [id, d] : ctl.prefs().deviceLighting) d.own = false;  // every device the same look
            ctl.Changed();
        }
        ImGui::PushFont(f.bold);
        ImGui::TextUnformatted("On your devices");
        ImGui::PopFont();
        SetupCanvas(ctl, ui, false);
        ImGui::Dummy(ImVec2(0, 6 * S()));
        ImGui::PushFont(f.bold);
        ImGui::TextUnformatted("While you play");
        ImGui::PopFont();
        int mode = ui.setupAuto ? 0 : 1;
        const char* modes[] = {"Games light everything (recommended)", "Always my look"};
        if (Segmented("setupmode", &mode, modes, 2, colW)) ui.setupAuto = mode == 0;
        Muted("%s", ui.setupAuto ? "Games with lighting light your devices while they run; your look shows the rest of "
                                   "the time."
                                 : "Your look stays on, games or not.");
        if (footer(false, "Finish")) SetupFinish(ctl, ui);
        break;
    }
    default: {
        SetupRun(ctl, in, ui);
        const bool done = ui.setupNext >= setup::kConns;
        int failed = 0;
        for (auto s : ui.setupState) failed += s == SetupState::Failed;
        heading(done ? (failed ? "Almost done" : "All set") : "Setting up...",
                done ? (failed ? "Some connections couldn't be set up; you can try them again on the Integrations "
                                 "page (Settings has the log folder)."
                               : "Your lighting is ready. Everything can be changed later on the Devices and "
                                 "Integrations pages.")
                     : "Approve each Windows administrator prompt as it comes.");
        for (int i = 0; i < setup::kConns; ++i) {
            const SetupState s = ui.setupState[i];
            const unsigned col = s == SetupState::Done      ? kGreen
                                 : s == SetupState::Failed  ? kRed
                                 : s == SetupState::Running ? kAccent
                                                            : kMuted;
            ImGui::PushID(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetTextLineHeight();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 c(p.x + h / 2, p.y + h / 2);
            if (s == SetupState::Running) {
                const float a0 = static_cast<float>(ImGui::GetTime() * 6);
                dl->PathArcTo(c, h / 2 - 1 * S(), a0, a0 + 4.5f, 16);
                dl->PathStroke(Hex(col), 0, 2 * S());
            } else {
                dl->AddCircleFilled(c, h / 2 - 2 * S(), Hex(col, s == SetupState::Pending ? 90 : 255), 16);
            }
            ImGui::Dummy(ImVec2(h, h));
            ImGui::SameLine(0, 10 * S());
            ImGui::TextUnformatted(setup::Info(static_cast<Conn>(i)).name);
            ImGui::SameLine(colW * 0.45f);
            ImGui::PushStyleColor(ImGuiCol_Text, V4(col == kMuted ? kMuted : col));
            ImGui::PushTextWrapPos(0);
            ImGui::TextWrapped("%s", ui.setupResult[i].c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (done) {
            ImGui::Dummy(ImVec2(0, 12 * S()));
            const float nw = 150 * S();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - nw);
            if (PrimaryButton("Next", ImVec2(nw, 0))) ui.setupStep = kStepLook;
        }
        break;
    }
    }
    ImGui::EndChild();
}

void DrawUi(HWND hwnd, Controller& ctl, Integrations& integrations, UiState& ui, const Fonts& f) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("LumaBridge", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    integrations.SetOwner(hwnd);  // administrator prompts open in front of LumaBridge, from any page
    // The setup guide fills the window until it's finished (first start, or from Settings).
    if (!ctl.prefs().setupDone && ui.setupStep < 0) ui.setupStep = 0;
    if (ui.setupStep >= 0) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * S(), 22 * S()));
        ImGui::BeginChild("setup-guide", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        SetupGuide(ctl, integrations, ui, f);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();
        Splash(ui, f);
        return;
    }
    const float sidebarW = 210 * S();
    Sidebar(ctl, ui, f, sidebarW);
    ImGui::SameLine(0, 0);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * S(), 22 * S()));
    ImGui::BeginChild("main", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);

    // Header: page title + global mode switch.
    const char* titles[] = {"Dashboard", "Lighting", "Games List", "Devices", "Integrations", "Settings"};  // Page order
    const char* subtitles[] = {"Your system and lighting at a glance",
                               "Auto follows your games; Manual is your own look",
                               "Every game on this PC, and how it lights up",
                               "Aura, fans, RAM, keyboard and mouse",
                               "Answer games as the lighting software they look for",
                               "Calibration, startup and more"};
    ImGui::BeginGroup();
    ImGui::PushFont(f.title);
    ImGui::TextUnformatted(titles[static_cast<int>(ui.page)]);
    ImGui::PopFont();
    ImGui::PushFont(f.caption);
    Muted("%s", subtitles[static_cast<int>(ui.page)]);
    ImGui::PopFont();
    ImGui::EndGroup();
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
    {
        // Accent line under the header.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(p, ImVec2(p.x + w * 0.35f, p.y + 2 * S()), Hex(kAccent),
                                                           Hex(kAccent2, 0), Hex(kAccent2, 0), Hex(kAccent));
        ImGui::Dummy(ImVec2(w, 2 * S()));
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
    if (ui.page == Page::GamesList && ui.lastPage != Page::GamesList) ui.gameDetail.clear();  // open on the list
    if (ui.page == Page::Devices && ui.lastPage != Page::Devices) ui.deviceDetail.clear();
    if (ui.page != Page::Devices && ctl.fanTest()) ctl.SetFanTest(false);  // the test is on the fans' page
    ui.lastPage = ui.page;

    switch (ui.page) {
    case Page::Lighting:
        if (ctl.prefs().mode == Mode::Auto) AutoPage(ctl, ui, f);
        else ManualPage(ctl, ui, f);
        break;
    case Page::Devices: DevicesPage(ctl, integrations, ui, f); break;
    case Page::Dashboard: DashboardPage(hwnd, ctl, integrations, ui, f); break;
    case Page::GamesList: GamesListPage(hwnd, ctl, integrations, ui, f); break;
    case Page::Integrations: IntegrationsPage(hwnd, ctl, integrations, ui, f); break;
    case Page::Settings: SettingsPage(ctl, ui, f); break;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
    Splash(ui, f);
}

}  // namespace luma::app
