#include "ui.h"
#include "notifications.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <functional>
#include <map>
#include <set>
#include <iterator>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "controller.h"
#include "controller_model.h"
#include "scene_gpu.h"
#include "desk_models.h"
#include "pc_model.h"
#include "monitor_model.h"
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
#include "nzxt_kraken.h"
#include "perf_history.h"
#include "pc_layout.h"
#include "displays.h"

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
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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

// ImGui::Button, with the hand cursor on hover that Dear ImGui doesn't set by itself.
bool Btn(const char* label, ImVec2 size = ImVec2(0, 0)) {
    const bool r = ImGui::Button(label, size);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return r;
}

// ImGui::SmallButton, with the hand cursor on hover.
bool SmallBtn(const char* label) {
    const bool r = ImGui::SmallButton(label);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return r;
}

bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, V4(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V4(kAccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, V4(kAccent));
    ImGui::PushStyleColor(ImGuiCol_Text, V4(0xFFFFFF));
    bool r = Btn(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// Which way moving effects run: a left and a right arrow, the chosen one lit. `*reversed`
// false = right (the normal way). Returns true when it changed.
bool DirectionArrows(const char* id, bool* reversed) {
    bool changed = false;
    ImGui::PushID(id);
    const float pad = 3 * S();
    const float h = ImGui::GetFrameHeight() + 2 * S();
    const float bw = 44 * S(), bh = h - pad * 2;
    const float total = bw * 2 + pad * 2;
    ImGui::AlignTextToFramePadding();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + total, p.y + h), Hex(kTrack), h / 2);
    dl->AddRect(p, ImVec2(p.x + total, p.y + h), Hex(kBorder, 160), h / 2);
    for (int i = 0; i < 2; ++i) {
        const bool right = i == 1;
        const ImVec2 a(p.x + pad + i * bw, p.y + pad);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("dir", ImVec2(bw, bh));
        ImGui::PopID();
        const bool sel = *reversed != right, hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (hovered) ImGui::SetTooltip(right ? "Right" : "Left");
        if (sel) dl->AddRectFilled(a, ImVec2(a.x + bw, a.y + bh), Hex(hovered ? kAccentHover : kAccent), bh / 2);
        else if (hovered) dl->AddRectFilled(a, ImVec2(a.x + bw, a.y + bh), Hex(kCardHover), bh / 2);
        // The arrow: a shaft and a head, pointing left or right.
        const ImU32 col = sel ? Hex(0xFFFFFF) : hovered ? Hex(kText) : Hex(kMuted);
        const ImVec2 c(a.x + bw / 2, a.y + bh / 2);
        const float len = 9 * S(), head = 5 * S(), t = std::max(1.5f, 2 * S());
        const float dir = right ? 1.f : -1.f;
        dl->AddLine(ImVec2(c.x - dir * len, c.y), ImVec2(c.x + dir * (len - 1 * S()), c.y), col, t);
        dl->AddTriangleFilled(ImVec2(c.x + dir * (len + 2 * S()), c.y), ImVec2(c.x + dir * (len - head), c.y - head),
                              ImVec2(c.x + dir * (len - head), c.y + head), col);
        if (clicked && !sel) {
            *reversed = !right;
            changed = true;
        }
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(total, h));
    ImGui::PopID();
    return changed;
}

// "Direction  [<-|->]" on one line.
bool DirectionRow(const char* id, bool* reversed, const char* label = "Direction") {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0, 12 * S());
    return DirectionArrows(id, reversed);
}

bool Swatch(const char* id, Rgb c, float size, bool selected = false) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("sw", ImVec2(size, size));
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
    Disk, Bolt, Gauge, Sun, Tower
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
    case Icon::Tower:  // a PC case with a fan in its front
        dl->AddRect(At(c, -r * 0.55f, -r * 0.92f), At(c, r * 0.55f, r * 0.92f), col, r * 0.1f, 0, t);
        dl->AddCircle(At(c, 0, -r * 0.28f), r * 0.28f, col, 16, t);
        dl->AddCircle(At(c, 0, r * 0.38f), r * 0.28f, col, 16, t);
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

// ---- Graphs: the last two minutes of each figure ------------------------------------------

struct PerfHistory {
    perf::Series cpuLoad, cpuTemp, gpuLoad, mem, power;
    double at = -1;  // when the last sample was taken (ImGui time)
};
PerfHistory& History() {
    static PerfHistory h;
    return h;
}

// One figure over time: a thin line over a faint area, newest on the right, on a recessive
// baseline; hover for the value then. `lo`..`hi` is its scale.
void Graph(const char* id, const perf::Series& series, double lo, double hi, unsigned color, const char* fmt) {
    if (!ImGui::GetCurrentContext() || !series.Any()) return;
    const float w = ImGui::GetContentRegionAvail().x, h = 38 * S();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& v = series.values();
    const size_t n = perf::Series::kLength;
    const size_t off = n - v.size();  // a short history starts from the right
    auto X = [&](size_t i) { return a.x + w * static_cast<float>(off + i) / static_cast<float>(n - 1); };
    auto Y = [&](double val) {
        const double t = std::clamp((val - lo) / std::max(1e-9, hi - lo), 0.0, 1.0);
        return a.y + h - 1 - static_cast<float>(t) * (h - 3 * S());
    };
    dl->AddLine(ImVec2(a.x, a.y + h - 0.5f), ImVec2(a.x + w, a.y + h - 0.5f), Hex(kBorder), 1.f);
    // The area, then the line, in runs between gaps.
    for (size_t i = 0; i + 1 < v.size(); ++i) {
        if (v[i] < 0 || v[i + 1] < 0) continue;
        const ImVec2 p0(X(i), Y(v[i])), p1(X(i + 1), Y(v[i + 1]));
        dl->AddQuadFilled(p0, p1, ImVec2(p1.x, a.y + h - 1), ImVec2(p0.x, a.y + h - 1), Hex(color, 34));
    }
    for (size_t i = 0; i + 1 < v.size(); ++i)
        if (v[i] >= 0 && v[i + 1] >= 0)
            dl->AddLine(ImVec2(X(i), Y(v[i])), ImVec2(X(i + 1), Y(v[i + 1])), Hex(color), 2 * S());
    if (hovered) {
        // The sample under the mouse: a crosshair, a dot and its value.
        const float mx = ImGui::GetIO().MousePos.x;
        size_t best = v.size();
        float bestD = 1e9f;
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] >= 0 && std::fabs(X(i) - mx) < bestD) bestD = std::fabs(X(i) - mx), best = i;
        if (best < v.size()) {
            const float x = X(best), y = Y(v[best]);
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, a.y + h), Hex(kMuted, 120), 1.f);
            dl->AddCircleFilled(ImVec2(x, y), 4 * S(), Hex(kCard), 12);
            dl->AddCircleFilled(ImVec2(x, y), 3 * S(), Hex(color), 12);
            char val[48];
            snprintf(val, sizeof val, fmt, v[best]);
            const int ago = static_cast<int>(v.size() - 1 - best);
            if (ago == 0) ImGui::SetTooltip("%s  now", val);
            else ImGui::SetTooltip("%s  %d s ago", val, ago);
        }
    }
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
    if (c.ctl.prefs().dashGraphs) Graph("cpu-graph", History().cpuLoad, 0, 100, kAccent, "%.0f %% load");
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

// The graphics card: the one with the most memory of its own (not the processor's built-in one).
const sensors::GpuStat* MainGpuOf(const sensors::SystemSnapshot& snap) {
    const sensors::GpuStat* card = nullptr;
    for (const auto& g : snap.gpus)
        if (!card || g.vramTotal > card->vramTotal) card = &g;
    return card;
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
            if (c.ctl.prefs().dashGraphs && &g == MainGpuOf(s)) Graph("gpu-graph", History().gpuLoad, 0, 100, kAccent, "%.0f %% load");
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
        if (c.ctl.prefs().dashGraphs) Graph("mem-graph", History().mem, 0, 100, kAccent, "%.0f %% used");
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
    // An NZXT Kraken reports its pump and radiator fans itself.
    if (const nzxt::Status k = c.ctl.kraken(); k.valid) {
        auto kRow = [&](int rpm, const char* what) {
            ImGui::PushID(shown++);
            IconItem(Icon::Fan, 18 * S(), Hex(kAccent), t * static_cast<float>(rpm) / 60.f * 0.35f);
            ImGui::SameLine();
            ImGui::Text("%d RPM", rpm);
            ImGui::SameLine();
            Muted("%s", what);
            ImGui::PopID();
        };
        kRow(k.pumpRpm, "Kraken pump");
        if (k.fanRpm > 0) kRow(k.fanRpm, "Kraken radiator fans");
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
                                            {"Tctl", "Package", "CPU"})) {
        row("CPU", x->value);
        if (c.ctl.prefs().dashGraphs) Graph("temp-graph", History().cpuTemp, 20, 100, kAccent2, "%.0f \xC2\xB0" "C CPU");
    }
    for (const auto& g : s.gpus)
        if (g.temp >= 0) row(sensors::FriendlyGpu(g.name).c_str(), g.temp);
    if (const nzxt::Status k = c.ctl.kraken(); k.valid) {
        // Liquid runs cooler than chips: its own colors (green below 40, amber to 60, then red).
        ImGui::PushStyleColor(ImGuiCol_Text, V4(TempColor(k.liquidC + 20)));
        ImGui::Text("%5.1f \xC2\xB0" "C", k.liquidC);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        Muted("Liquid (NZXT Kraken)");
        ++shown;
    }
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

bool HasDualSense(Controller& ctl) {
    const auto& p = ctl.presence();
    return p.scanned ? p.dualsense || ctl.dualsense().state() == DualSenseOutput::State::Active
                      : ctl.prefs().dualsenseController;
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

// The processor's and the graphics cards' power right now (W; -1: not reported).
const sensors::Sensor* CpuPower(const sensors::SystemSnapshot& s) {
    return sensors::PickSensor(s.lhm, sensors::HardwareKind::Cpu, sensors::SensorType::Power, {"Package", "CPU Package", "Core"});
}
std::vector<std::pair<std::string, double>> GpuPowers(const sensors::SystemSnapshot& s) {
    std::vector<std::pair<std::string, double>> out;
    for (const auto& g : s.gpus) {
        double w = g.powerW;
        if (w < 0)
            if (const auto* x = sensors::PickSensor(s.lhm, sensors::HardwareKind::Gpu, sensors::SensorType::Power, {"Package", "Board", "Core"}))
                w = x->value;
        if (w >= 0) out.push_back({sensors::FriendlyGpu(g.name), w});
    }
    return out;
}

// How many fans the case has (as set on My setup, else a guess of three).
int CaseFans(const Prefs& p) {
    pc::Layout l;
    return pc::Decode(p.caseLayout, &l) ? l.Fans() : 3;
}

// What the whole PC draws from the power supply, estimated (perf_history.h).
perf::Draw SystemDraw(Controller& ctl, const sensors::SystemSnapshot& s) {
    const auto* cpu = CpuPower(s);
    double gpu = -1;
    for (const auto& g : GpuPowers(s)) gpu = std::max(0.0, gpu) + g.second;
    return perf::EstimateDraw(cpu ? cpu->value : -1, gpu, static_cast<int>(Drives().size()), CaseFans(ctl.prefs()));
}

// The power supply's rating: a supply that reports itself over USB, else what you set.
int PsuWatts(Controller& ctl, std::string* name) {
    if (const catalog::PsuModel* m = catalog::FindPsu(ctl.presence().usb)) {
        if (name) *name = m->name;
        return m->watts;
    }
    if (name) *name = "Power supply";
    return ctl.prefs().psuWatts;
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
    char b[96];
    const auto* cpuW = CpuPower(s);
    const std::vector<std::pair<std::string, double>> gpuW = GpuPowers(s);
    if (cpuW) total += cpuW->value, ++parts;
    for (const auto& g : gpuW) total += g.second, ++parts;
    // The whole PC against the power supply's rating.
    std::string psuName;
    const int rated = PsuWatts(c.ctl, &psuName);
    const perf::Draw draw = SystemDraw(c.ctl, s);
    if (draw.total >= 0) {
        snprintf(b, sizeof b, "%.0f", draw.total);
        char unit[48];
        if (rated > 0) snprintf(unit, sizeof unit, "W of %d W", rated);
        else snprintf(unit, sizeof unit, "W, the whole PC");
        const double load = perf::PsuLoad(draw.total, rated);
        const unsigned col = load < 0 ? kText : load < 0.7 ? kText : load < 0.9 ? kAmber : kRed;
        Metric(c.f, b, unit, col);
        if (load >= 0) {
            Bar(load, load < 0.7 ? kAccent : load < 0.9 ? kAmber : kRed);
            Muted("%s: %.0f %% of its rating", psuName.c_str(), load * 100);
        }
        if (c.ctl.prefs().dashGraphs)
            Graph("power-graph", History().power, 0, std::max({rated > 0 ? rated * 0.6 : 0.0, History().power.Max() * 1.25, 150.0}), kAccent2,
                  "%.0f W");
        ImGui::Dummy(ImVec2(0, 2 * S()));
    } else if (parts) {
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
    // Clocks: the fastest core (LibreHardwareMonitor), else Windows' own effective clock; the
    // graphics core (NVIDIA's library, else LibreHardwareMonitor).
    double cpuClock = -1;
    for (const auto& x : s.lhm)
        if (x.kind == HardwareKind::Cpu && x.type == SensorType::Clock && x.name.find("Core") != std::string::npos)
            cpuClock = std::max(cpuClock, x.value);
    if (cpuClock <= 0) cpuClock = s.cpuClockMhz;
    if (cpuClock > 0) {
        snprintf(b, sizeof b, "%.2f GHz", cpuClock / 1000);
        row(Icon::Gauge, "Processor clock", b);
    }
    for (const auto& g : s.gpus) {
        double mhz = g.clockMhz;
        if (mhz <= 0)
            if (const auto* x = sensors::PickSensor(s.lhm, HardwareKind::Gpu, SensorType::Clock, {"GPU Core", "Core"}))
                mhz = x->value;
        if (mhz <= 0) continue;
        snprintf(b, sizeof b, "%.0f MHz", mhz);
        row(Icon::Gauge, s.gpus.size() > 1 ? sensors::FriendlyGpu(g.name) + " clock" : std::string("Graphics clock"), b);
    }
    if (draw.total >= 0) {
        snprintf(b, sizeof b, "~%.0f W", draw.rest);
        row(Icon::Board, "Rest of the PC (estimate)", b);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The motherboard, memory, drives, fans and USB report no power: about 35 W, 5 W a drive and 2 W a fan.");
    }
    // The power supply's rating: set here unless it reports itself over USB.
    if (!catalog::FindPsu(c.ctl.presence().usb)) {
        ImGui::AlignTextToFramePadding();
        IconItem(Icon::Bolt, 16 * S(), Hex(kMuted));
        ImGui::SameLine();
        ImGui::TextUnformatted("Power supply");
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 118 * S());
        int w = c.ctl.prefs().psuWatts;
        ImGui::SetNextItemWidth(118 * S());
        const int step = 50, fast = 100;
        if (ImGui::InputScalar("##psu", ImGuiDataType_S32, &w, &step, &fast, w > 0 ? "%d W" : "%d")) {
            c.ctl.prefs().psuWatts = std::clamp(w, 0, 5000);
            c.ctl.Changed();
        }
        if (c.ctl.prefs().psuWatts <= 0) Muted("Enter your power supply's watts (on its label) to see how much of it the PC uses.");
    }
    if (!cpuW) {
        // Why the processor's power is missing.
        if (!s.lhmConnected) Muted("Processor power: set up Hardware access on the Devices page (one click).");
        else if (s.sensorSource.rfind("LumaBridge", 0) == 0)
            Muted("Processor power: update Hardware access on the Devices page (one click) to read it.");
    }
}

// Once a second, whatever page is open: each graphed figure's value (-1 when not reported).
void SampleHistory(Controller& ctl) {
    PerfHistory& h = History();
    const double now = ImGui::GetTime();
    if (h.at >= 0 && now - h.at < 1.0) return;
    h.at = now;
    const sensors::SystemSnapshot s = ctl.monitor().Snapshot();
    if (!s.ready) return;
    h.cpuLoad.Push(s.cpuLoad);
    const auto* t = sensors::PickSensor(s.lhm, sensors::HardwareKind::Cpu, sensors::SensorType::Temperature, {"Tctl", "Package", "Core (Tdie)", "CPU"});
    h.cpuTemp.Push(t ? t->value : -1);
    double gpu = -1;
    if (const sensors::GpuStat* g = MainGpuOf(s)) {
        gpu = g->load;
        if (gpu < 0)
            if (const auto* x = sensors::PickSensor(s.lhm, sensors::HardwareKind::Gpu, sensors::SensorType::Load, {"Core"})) gpu = x->value;
    }
    h.gpuLoad.Push(gpu);
    h.mem.Push(s.memTotal ? 100.0 * static_cast<double>(s.memUsed) / static_cast<double>(s.memTotal) : -1);
    h.power.Push(SystemDraw(ctl, s).total);
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

// Count active lighting devices across every output, including devices outside Aura.
// Shared controllers (an ARGB header or the RAM helper) count as one lighting group.
int LitDevices(Controller& ctl) {
    if (ctl.output().stopped) return 0;
    int n = 0;
    const auto& prefs = ctl.prefs();
    const auto& disabled = ctl.config().auraDisabledDevices;
    if (ctl.auraStatus().connected)
        for (const auto& d : ctl.devices()) {
            const char* id = d.type == 0x00011000 ? device::kFans : device::kBoard;
            if (DeviceNative(prefs, id)) continue;
            const bool off = std::any_of(disabled.begin(), disabled.end(), [&](const std::wstring& name) {
                return _wcsicmp(name.c_str(), d.name.c_str()) == 0;
            });
            n += !off;
        }
    if (prefs.logitechDevices && !DeviceNative(prefs, device::kMouse) &&
        ctl.logitech().state() == LogitechOutput::State::Active && !ctl.logitechAsleep())
        n += static_cast<int>(LogitechRgb(ctl).size());
    if (prefs.ramLighting && !DeviceNative(prefs, device::kRam) &&
        ctl.hardware().ramState() == HardwareHelper::RamState::Active) ++n;
    if (prefs.azothKeyboard && !DeviceNative(prefs, device::kKeyboard) &&
        ctl.azoth().state() == AzothOutput::State::Active && !ctl.azothAsleep()) ++n;
    if (prefs.dualsenseController && !DeviceNative(prefs, device::kController) &&
        ctl.dualsense().state() == DualSenseOutput::State::Active) ++n;
    if (!DeviceNative(prefs, device::kOther)) {
        if (ctl.lampArray().state() == LampArrayOutput::State::Running)
            for (const auto& d : LampArrayLit(ctl)) n += d.problem.empty() && d.lamps > 0;
        if (ctl.openRgb().state() == OpenRgbOutput::State::Connected)
            for (const auto& d : OpenRgbLit(ctl)) n += d.leds > 0;
        if (prefs.nzxtLighting && ctl.krakenLightingActive()) ++n;
    }
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
    const int lit = LitDevices(c.ctl);
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
    // Built-in lighting: the games on this PC first (green while their feed is live), the
    // rest LumaBridge supports below.
    const auto& feeds = c.ctl.feeds();
    auto seen = [&](games::Feed feed) {
        switch (feed) {
        case games::Feed::Cs2Gsi: return feeds.Cs2Seen();
        case games::Feed::RocketLeagueStats: return feeds.RocketLeagueConnected();
        case games::Feed::WarThunderApi: return feeds.WarThunderSeen();
        case games::Feed::Dota2Gsi: return feeds.Dota2Seen();
        case games::Feed::LeagueLiveClient: return feeds.LeagueSeen();
        case games::Feed::ForzaDataOut: return feeds.ForzaSeen();
        case games::Feed::FlightSimConnect: return feeds.FlightSimSeen();
        case games::Feed::DcsExport: return feeds.DcsSeen();
        case games::Feed::F1Telemetry: return feeds.F1Seen();
        default: return false;
        }
    };
    auto installed = [&](const games::GameProfile& prof) {
        for (const auto& g : c.ctl.library()) {
            if (games::FindProfile("", Utf8(g.name)) == &prof) return true;
            for (const auto& exe : g.exeNames)
                if (games::FindProfile(exe, "") == &prof) return true;
        }
        return false;
    };
    std::vector<std::pair<std::string, unsigned>> mine;
    std::string others;
    size_t count = 0;
    const games::GameProfile* all = games::Profiles(&count);
    for (size_t i = 0; i < count; ++i) {
        const games::GameProfile& prof = all[i];
        if (prof.kind != games::ProfileKind::BuiltIn) continue;
        if (installed(prof)) mine.push_back({prof.title, seen(prof.feed) ? kGreen : kAccent});
        else others += (others.empty() ? "" : ", ") + std::string(prof.title);
    }
    Muted("Your games with built-in lighting");
    if (mine.empty()) Muted("None found on this PC yet.");
    else PillFlow(mine);
    if (!others.empty()) {
        ImGui::Dummy(ImVec2(0, 2 * S()));
        Muted("Also supported: %s", others.c_str());
    }
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (Btn("Games list", ImVec2(120 * S(), 0))) c.ui.page = Page::GamesList;
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
    if (Btn("Reset to default")) {
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

    if (Btn(ui.dashEdit ? "Close" : "Customize", ImVec2(110 * S(), 0))) ui.dashEdit = !ui.dashEdit;
    ImGui::SameLine(0, 10 * S());
    ImGui::AlignTextToFramePadding();
    Muted("Hide and reorder the cards below.");
    ImGui::SameLine(0, 24 * S());
    if (Toggle("Graphs", &ctl.prefs().dashGraphs)) ctl.Changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The last two minutes of load, memory, temperature and power, under each figure.");
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

// The devices on this PC LumaBridge lights (device::kFans, ...), in the Lighting page's order.
std::vector<const char*> LitDeviceIds(Controller& ctl) {
    const Prefs& p = ctl.prefs();
    const sensors::SystemSnapshot snap = ctl.monitor().Snapshot();
    std::vector<const char*> out;
    for (const char* id : device::All()) {
        if ((id == std::string(device::kRam) && !(p.ramLighting && HasRgbRam(ctl, snap))) ||
            (id == std::string(device::kMouse) && !(p.logitechDevices && HasLogitechRgb(ctl))) ||
            (id == std::string(device::kKeyboard) && !(p.azothKeyboard && HasAzoth(ctl))) ||
            (id == std::string(device::kController) && !(p.dualsenseController && HasDualSense(ctl))) ||
            (id == std::string(device::kOther) && OpenRgbLit(ctl).empty() && LampArrayLit(ctl).empty()))
            continue;
        out.push_back(id);
    }
    return out;
}

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
        Muted("Applies to every device, in Auto and Manual mode.");
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
        Muted("For this device, games included. Overall brightness: %.0f%%.",
              ctl.config().auraCorrection.brightness * 100.0);
        ImGui::Dummy(ImVec2(0, 4 * S()));
        bool& rev = ctl.prefs().deviceLighting[device].reverse;
        if (DirectionRow("dev-dir", &rev)) ctl.Changed();
        Muted("Which way moving effects run on this device.");
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

void EffectStrip(const fx::Params& fx, ImVec2 a, ImVec2 b, float rounding);
bool IsRainbow(ManualEffect effect);
void SetupCanvas2d(Controller& ctl, UiState& ui, bool selectable, bool overview = false);

void AutoIdlePreview(Controller& ctl, UiState& ui, const Fonts& f) {
    const auto idle = ctl.prefs().idle;
    const bool external = idle == IdleBehavior::ArmouryCrate;
    fx::Params effect = ToParams(MainLook(ctl.prefs()));
    if (idle == IdleBehavior::Rainbow) {
        effect.kind = fx::Kind::RainbowWave;
        effect.speed = 0.1;
        effect.reverse = false;
    } else if (idle == IdleBehavior::Off) {
        effect = fx::Params{};
        effect.color1 = Rgb{};
    }
    BeginCard("idle-preview");
    CardTitle(f, "Between games", Icon::Palette);
    const char* names[] = {"My manual color", "Rainbow", "Lights off", "Armoury Crate"};
    Muted("%s", names[static_cast<int>(idle)]);
    ImGui::Dummy(ImVec2(0, 8 * S()));
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 104 * S();
    const ImVec2 b(a.x + w, a.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (external) {
        dl->AddRectFilled(a, b, Hex(kTrack), 12 * S());
        const char* text = "Its own lighting";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(a.x + (w - ts.x) / 2, a.y + (h - ts.y) / 2), Hex(kMuted), text);
    } else {
        EffectStrip(effect, a, b, 12 * S());
    }
    dl->AddRect(a, b, Hex(kBorder), 12 * S());
    ImGui::Dummy(ImVec2(w, h));
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (external) {
        Muted("Armoury Crate chooses the color when LumaBridge hands back control.");
    } else {
        ImGui::PushFont(f.bold);
        if (IsRainbow(effect.kind)) ImGui::TextUnformatted(kEffects[static_cast<int>(effect.kind)].name);
        else ImGui::TextUnformatted(ToHex(effect.color1).c_str());
        ImGui::PopFont();
        if (!IsRainbow(effect.kind))
            Muted("%s", idle == IdleBehavior::Off ? "Lights stay dark between games" : kEffects[static_cast<int>(effect.kind)].name);
        if (fx::UsesSecondColor(effect.kind)) Muted("Second color: %s", ToHex(effect.color2).c_str());
    }
    if (idle == IdleBehavior::ManualColor && PrimaryButton("Switch to Manual", ImVec2(w, 0))) {
        ctl.prefs().mode = Mode::Manual;
        ui.lightTarget.clear();
        ui.colorSlot = 0;
        ctl.Changed();
    }
    EndCard();
}

void AutoOverview(Controller& ctl, UiState& ui, const Fonts& f) {
    const float width = ImGui::GetContentRegionAvail().x;
    const bool side = width >= 760 * S();
    if (ImGui::BeginTable("auto-overview", side ? 2 : 1, ImGuiTableFlags_SizingStretchProp)) {
        if (side) {
            ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 280 * S());
            ImGui::TableSetupColumn("Setup", ImGuiTableColumnFlags_WidthStretch);
        }
        ImGui::TableNextColumn();
        AutoIdlePreview(ctl, ui, f);
        if (side) BrightnessCard(ctl, f);
        ImGui::TableNextColumn();
        BeginCard("auto-setup");
        CardTitle(f, "Your setup", Icon::Grid);
        Muted("Live lighting across your components");
        ImGui::Dummy(ImVec2(0, 6 * S()));
        SetupCanvas2d(ctl, ui, false, true);
        ImGui::Dummy(ImVec2(0, 4 * S()));
        if (!ctl.output().label.empty()) Muted("Now: %s", ctl.output().label.c_str());
        EndCard();
        if (!side) BrightnessCard(ctl, f);
        ImGui::EndTable();
    }
}

// The selected device's saved look stays visible above its editing controls.
void ManualColorCard(Controller& ctl, UiState& ui, const Fonts& f) {
    const Look look = ui.lightTarget.empty() ? MainLook(ctl.prefs()) : DeviceLook(ctl.prefs(), ui.lightTarget);
    const bool native = !ui.lightTarget.empty() && DeviceNative(ctl.prefs(), ui.lightTarget);
    BeginCard("manual-summary");
    CardTitle(f, "Your manual lighting", Icon::Palette);
    Muted("%s", ui.lightTarget.empty() ? "All devices" : device::Name(ui.lightTarget));
    ImGui::Dummy(ImVec2(0, 8 * S()));
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 104 * S();
    const ImVec2 b(a.x + w, a.y + h);
    if (native) {
        ImGui::GetWindowDrawList()->AddRectFilled(a, b, Hex(kTrack), 12 * S());
        const char* text = "Its own lighting";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + (w - ts.x) / 2, a.y + (h - ts.y) / 2), Hex(kMuted), text);
    } else {
        EffectStrip(ToParams(look), a, b, 12 * S());
    }
    ImGui::GetWindowDrawList()->AddRect(a, b, Hex(kBorder), 12 * S());
    ImGui::Dummy(ImVec2(w, h));
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (native) {
        Muted("Choose a lighting source below to bring this device back under LumaBridge control.");
    } else {
        ImGui::PushFont(f.title);
        ImGui::TextUnformatted(IsRainbow(look.effect) ? kEffects[static_cast<int>(look.effect)].name : ToHex(look.color1).c_str());
        ImGui::PopFont();
        if (!IsRainbow(look.effect)) {
            Muted("%s", kEffects[static_cast<int>(look.effect)].name);
            Muted("RGB %u, %u, %u", unsigned(look.color1.r), unsigned(look.color1.g), unsigned(look.color1.b));
        }
        if (fx::UsesSecondColor(look.effect)) Muted("Second color: %s", ToHex(look.color2).c_str());
        Muted("Choose your color and effect below.");
    }
    EndCard();
}

void AutoGamesCard(Controller& ctl, const Fonts& f) {
    BeginCard("now");
    CardTitle(f, "Dynamic lighting", Icon::Game);
    Muted("Games drive your lights. When several are running, the one that changed color most recently wins.");
    ImGui::Dummy(ImVec2(0, 8 * S()));

    const auto& running = ctl.games();
    const auto others = ctl.unmatchedSources();
    if (running.empty() && others.empty()) {
        Pill("No game running", kMuted);
        ImGui::Dummy(ImVec2(0, 4 * S()));
        Muted("Your games appear here when they start. Until then, your between-games lighting is used.");
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
    ImGui::Dummy(ImVec2(0, 6 * S()));
    if (Toggle("Use screen colors for games without lighting", &ctl.prefs().screenForUnsupported)) ctl.Changed();
    Muted("Uses the screen image. Choose exceptions on the Games List page.");
    EndCard();
}

void AutoPage(Controller& ctl, UiState& ui, const Fonts& f) {
    AutoOverview(ctl, ui, f);
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
    EndCard();

    if (ctl.prefs().idle == IdleBehavior::Rainbow) MainRainbowCard(ctl, f);
    AutoGamesCard(ctl, f);
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
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
    if (Btn("Reset rainbow")) {
        p.hueStart = 0;
        p.hueSpan = 360;
        p.saturation = 1;
        p.spread = 1;
        changed = true;
    }
    EndCard();
    return changed;
}

// Edit the color or palette first, then the effect, presets and speed. Returns
// true when it changed.
bool LookEditor(Controller& ctl, UiState& ui, const Fonts& f, Look& p) {
    bool changed = false;
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
            if (Btn("Swap", ImVec2(swapW, 44 * S()))) {
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
        if (DirectionRow("look-dir", &p.reverse)) changed = true;
    }
    EndCard();

    return changed;
}

// ---- Your setup: every device with its live effect, placed like the real thing -----------

// What a device shows right now (grey while LumaBridge isn't controlling the lights).
const fx::Params* LiveParams(const Controller& ctl, const char* id) {
    if (ctl.output().stopped || DeviceNative(ctl.prefs(), id)) return nullptr;  // grey: not LumaBridge's
    static std::map<std::string, fx::Params> live;  // stable addresses, one per device
    return &(live[id] = ctl.DeviceEffect(id));
}

// Also dimmed like the device: the overall brightness times the device's own.
Rgb LiveAt(const fx::Params* p, double t, int i, int n, double level = 1.0) {
    return p ? Scale(fx::Render(*p, t, i, n), level) : Rgb{50, 54, 64};
}

double LiveLevel(Controller& ctl, const char* id) {
    return ctl.config().auraCorrection.brightness * DeviceBrightness(ctl.prefs(), id);
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
    Muted("%s Click a slot to put a stick in or take it out (A1 is nearest the CPU, as printed on the board).",
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
    // The board around the socket, as you see it with the side panel off: the CPU, then the
    // four slots outward. A stick sits in a filled slot; click a slot to put one in or take it out.
    std::array<bool, 4> slots = RamSlotsShown(ctl, hw);
    static const char* kNames[] = {"A1", "A2", "B1", "B2"};
    const float h = 200 * S(), slotW = 26 * S(), gap = 14 * S(), pairGap = 26 * S(), cpu = 110 * S();
    const float w = 24 * S() + cpu + 36 * S() + 4 * slotW + 2 * gap + pairGap + 24 * S();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), Hex(0x151A22), 10 * S());
    dl->AddRect(o, ImVec2(o.x + w, o.y + h), Hex(0x262D3A), 10 * S());
    // The CPU socket.
    const ImVec2 ca(o.x + 24 * S(), o.y + h / 2 - cpu / 2), cb(ca.x + cpu, ca.y + cpu);
    dl->AddRectFilled(ca, cb, Hex(0x222834), 6 * S());
    dl->AddRect(ImVec2(ca.x + 10 * S(), ca.y + 10 * S()), ImVec2(cb.x - 10 * S(), cb.y - 10 * S()), Hex(0x3A4252), 4 * S(), 0, 1.5f * S());
    const ImVec2 cs = ImGui::CalcTextSize("CPU");
    dl->AddText(ImVec2((ca.x + cb.x) / 2 - cs.x / 2, (ca.y + cb.y) / 2 - cs.y / 2), Hex(kMuted), "CPU");
    const Rgb stick = ctl.prefs().ramLighting ? Rgb{124, 108, 255} : Rgb{90, 96, 110};
    float x = cb.x + 36 * S();
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        if (i == 2) x += pairGap - gap;
        const ImVec2 a(x, o.y + 30 * S()), b(x + slotW, o.y + h - 16 * S());
        ImGui::SetCursorScreenPos(ImVec2(a.x - 4 * S(), o.y + 6 * S()));
        ImGui::PushID(i);
        const bool click = ImGui::InvisibleButton("slot", ImVec2(slotW + 8 * S(), h - 16 * S()));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool full = slots[static_cast<size_t>(i)];
        // The slot: a long socket with a latch at each end.
        dl->AddRectFilled(ImVec2(a.x + 6 * S(), a.y), ImVec2(b.x - 6 * S(), b.y), Hex(0x0B0E13), 3 * S());
        dl->AddRectFilled(ImVec2(a.x + 4 * S(), a.y - 6 * S()), ImVec2(b.x - 4 * S(), a.y + 4 * S()), Hex(0x3A4252), 2 * S());
        dl->AddRectFilled(ImVec2(a.x + 4 * S(), b.y - 4 * S()), ImVec2(b.x - 4 * S(), b.y + 6 * S()), Hex(0x3A4252), 2 * S());
        if (full) {
            // A stick: its heat spreader, with a light bar along the top when RAM lighting is on.
            dl->AddRectFilled(ImVec2(a.x, a.y + 4 * S()), ImVec2(b.x, b.y - 4 * S()), Hex(0x2B313D), 4 * S());
            dl->AddRectFilled(ImVec2(a.x + 3 * S(), a.y + 8 * S()), ImVec2(b.x - 3 * S(), b.y - 8 * S()), Hex(0x39414F), 3 * S());
            dl->AddRectFilled(ImVec2(a.x, a.y + 4 * S()), ImVec2(a.x + 5 * S(), b.y - 4 * S()), Col(stick, ctl.prefs().ramLighting ? 230 : 120), 2 * S());
            for (int k = 0; k < 4; ++k) {  // chips
                const float cy = a.y + 24 * S() + static_cast<float>(k) * (b.y - a.y - 48 * S()) / 3.f;
                dl->AddRectFilled(ImVec2(a.x + 9 * S(), cy - 7 * S()), ImVec2(b.x - 5 * S(), cy + 7 * S()), Hex(0x1C2029), 2 * S());
            }
        } else if (hovered) {
            dl->AddRect(ImVec2(a.x, a.y + 4 * S()), ImVec2(b.x, b.y - 4 * S()), Hex(kAccent, 160), 4 * S(), 0, 1.5f * S());
        }
        if (hovered) {
            dl->AddRect(ImVec2(a.x - 3 * S(), a.y - 9 * S()), ImVec2(b.x + 3 * S(), b.y + 9 * S()), Hex(kAccentHover, 200), 5 * S(), 0, 1.5f * S());
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("%s: %s. Click to %s.", kNames[i], full ? "a stick" : "empty", full ? "take it out" : "put a stick in");
        }
        const ImVec2 ns = ImGui::CalcTextSize(kNames[i]);
        dl->AddText(ImVec2((a.x + b.x) / 2 - ns.x / 2, o.y + 8 * S()), Hex(full ? kText : kMuted), kNames[i]);
        if (click) {
            slots[static_cast<size_t>(i)] = !full;
            changed = true;
        }
        x += slotW + gap;
    }
    if (changed) {
        ctl.prefs().ramSlots = (slots[0] ? 1 : 0) | (slots[1] ? 2 : 0) | (slots[2] ? 4 : 0) | (slots[3] ? 8 : 0);
        ctl.Changed();
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h));
    ImGui::Dummy(ImVec2(w, 6 * S()));
    const int n = static_cast<int>(slots[0]) + slots[1] + slots[2] + slots[3];
    Muted("%d stick%s. Most boards want two in A2 and B2.", n, n == 1 ? "" : "s");
    if (ctl.prefs().ramSlots >= 0) {
        ImGui::SameLine(0, 16 * S());
        if (Btn("As the board says")) {
            ctl.prefs().ramSlots = -1;
            ctl.Changed();
        }
    }
    EndCard();
}

// ---- The setup in 3D (the Lighting page, My setup, the setup guide) ---------------------
// Everything LumaBridge found, drawn with the small renderer in scene3d.h: the case with its
// parts inside (they never leave it), and the desk with the monitor, keyboard, mouse and the
// other lit devices. LEDs show the live colors; fans spin with the speeds the sensors report.

namespace view3d {

// What a face belongs to (for picking, outlines and tooltips).
enum Obj : int {
    kNone = -1,
    kCase = 1, kBoard, kCpu, kRam, kGpu, kPsu, kStorage,
    kDesk = 20, kKeyboard, kMouse, kHeadset, kController,
    kMonitor0 = 40,  // + display (up to 8)
    kFan0 = 100,    // + case slot
    kOther0 = 200,  // + index into Model::others
};

constexpr int kMaxScreens = 8;
bool IsMonitor(int obj) { return obj >= kMonitor0 && obj < kMonitor0 + kMaxScreens; }
bool InCase(int obj) { return (obj >= kCase && obj <= kStorage) || (obj >= kFan0 && obj < kFan0 + pc::kSlots); }

uint32_t C(Rgb c, int a = 255) { return s3d::Rgba(c.r, c.g, c.b, a); }
uint32_t H(unsigned rgb, int a = 255) { return s3d::Rgba(static_cast<int>(rgb >> 16 & 0xFF), static_cast<int>(rgb >> 8 & 0xFF), static_cast<int>(rgb & 0xFF), a); }
const Rgb kDark{70, 76, 90};  // an LED LumaBridge isn't lighting

// A device on the desk.
struct Gear {
    int obj;             // kKeyboard, kMouse, kHeadset or kOther0 + i
    std::string name;
    const char* device;  // whose lighting it shows (device::k*), nullptr: no RGB LumaBridge controls
    std::string spot;    // "desk:keyboard", ...
    int leds = 8;        // for light bars
};

// What this PC has, gathered once per frame.
struct Model {
    pc::Layout layout;
    std::array<double, pc::kSlots> fanRpm{};  // -1: not reported
    double cpuFanRpm = -1, pumpRpm = -1, gpuFanPct = -1, cpuTemp = -1;
    const catalog::AioModel* aio = nullptr;  // the AIO cooler found on USB
    nzxt::Status kraken;                     // an NZXT Kraken's own readings
    std::array<bool, 4> ram{};
    bool ramRgb = false, boardRgb = false, fansRgb = false, gpuRgb = false;
    int boardLeds = 5, drives = 0;
    std::string boardName, cpuName, ramName, gpuName;
    Gear keyboard, mouse;
    bool headset = false;
    Gear headsetGear;
    bool controller = false;
    Gear controllerGear;
    std::vector<Gear> others;
    std::vector<displays::Display> screens;  // the monitors, as Windows has them arranged
    std::vector<std::string> notes;  // "Fan speeds: set up Hardware access", ...
};

// The monitors, looked up again every few seconds (Windows' arrangement can change).
const std::vector<displays::Display>& Screens() {
    static std::vector<displays::Display> list;
    static uint64_t at = 0;
    const uint64_t now = GetTickCount64();
    if (!at || now - at > 3000) {
        at = now;
        list = displays::List();
        if (list.size() > static_cast<size_t>(kMaxScreens)) list.resize(static_cast<size_t>(kMaxScreens));
    }
    return list;
}

bool Contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }
// Whether a device id (possibly none) is this one.
bool Is(const char* device, const char* id) { return device && std::strcmp(device, id) == 0; }

// The case layout: as corrected on My setup, else LumaBridge's guess from the ARGB header's
// fans and the fan headers that report a speed.
pc::Layout CaseLayout(Controller& ctl, const sensors::SystemSnapshot& snap, bool header, int* chassisHeaders) {
    int chassis = 0;
    for (const auto& x : snap.lhm)
        if (x.type == sensors::SensorType::Fan && x.value > 0 && !Contains(x.name, "CPU") && !Contains(x.name, "Pump") &&
            !Contains(x.name, "AIO"))
            ++chassis;
    *chassisHeaders = chassis;
    pc::Layout l;
    if (pc::Decode(ctl.prefs().caseLayout, &l)) return l;
    const int rgb = header ? ctl.config().argbFans.Fans() : 0;
    int plain = std::max(0, chassis - (rgb > 0 ? 1 : 0));  // the RGB fans usually share one header
    if (!rgb && !plain) plain = 2;
    l = pc::Guess(rgb, plain);
    // An AIO cooler's pump on USB, or a pump header that reports a speed: an AIO (its
    // radiator guessed on top).
    if (ctl.presence().Aio()) l.cooler = pc::Cooler::Aio;
    for (const auto& x : snap.lhm)
        if (x.type == sensors::SensorType::Fan && x.value > 0 && (Contains(x.name, "Pump") || Contains(x.name, "AIO")))
            l.cooler = pc::Cooler::Aio;
    return l;
}

Model Gather(Controller& ctl, const sensors::SystemSnapshot& snap) {
    Model m;
    const Prefs& p = ctl.prefs();
    bool header = false, board = false;
    for (const auto& d : ctl.devices()) {
        header |= d.type == 0x00011000;
        board |= d.type == 0x00010000;
    }
    int chassisHeaders = 0;
    m.layout = CaseLayout(ctl, snap, header, &chassisHeaders);
    m.fansRgb = header;
    m.boardRgb = board;
    m.boardLeds = BoardLedCount(ctl);
    // Fan speeds: the CPU fan, then the case fans from the other headers in order (RGB fans on
    // a hub share the first one).
    std::vector<double> chassis;
    for (const auto& x : snap.lhm) {
        if (x.type != sensors::SensorType::Fan) continue;
        if (Contains(x.name, "Pump") || Contains(x.name, "AIO")) m.pumpRpm = x.value;
        else if (Contains(x.name, "CPU") && !Contains(x.name, "OPT")) m.cpuFanRpm = x.value;
        else if (x.value > 0) chassis.push_back(x.value);
    }
    int plainK = 0;
    const bool rgbShare = m.layout.RgbFans() > 0 && !chassis.empty();
    for (int i = 0; i < pc::kSlots; ++i) {
        m.fanRpm[static_cast<size_t>(i)] = -1;
        const pc::SlotFan f = m.layout.slots[static_cast<size_t>(i)];
        if (f == pc::SlotFan::None || chassis.empty()) continue;
        if (f == pc::SlotFan::Rgb) m.fanRpm[static_cast<size_t>(i)] = chassis.front();
        else m.fanRpm[static_cast<size_t>(i)] = chassis[std::min(chassis.size() - 1, static_cast<size_t>(plainK++ + (rgbShare ? 1 : 0)))];
    }
    if (!snap.lhmConnected) m.notes.push_back("Fan speeds need Hardware access (Devices page); fans spin at a guess.");
    // The graphics card: the one with the most memory of its own (not the processor's built-in one).
    const sensors::GpuStat* card = nullptr;
    for (const auto& g : snap.gpus)
        if (!card || g.vramTotal > card->vramTotal) card = &g;
    if (card) {
        m.gpuName = sensors::FriendlyGpu(card->name);
        m.gpuFanPct = card->fanPct;
    }
    const SetupHardware hw = DetectSetup(snap.smbios);
    m.ram = RamSlotsShown(ctl, hw);
    m.ramRgb = p.ramLighting && HasRgbRam(ctl, snap);
    m.ramName = hw.ramName;
    m.boardName = hw.boardName.empty() ? sensors::FriendlyBoard(snap.smbios.boardMaker, snap.smbios.boardName) : hw.boardName;
    m.cpuName = sensors::FriendlyCpu(snap.cpuName);
    m.drives = static_cast<int>(Drives().size());
    m.screens = Screens();
    displays::ApplySizes(&m.screens, p.monitorSizes);
    m.aio = ctl.presence().Aio();
    if (const auto* x = sensors::PickSensor(snap.lhm, sensors::HardwareKind::Cpu, sensors::SensorType::Temperature,
                                            {"Tctl", "Package", "Core (Tdie)", "CPU"}))
        m.cpuTemp = x->value;
    // An NZXT Kraken reports its own pump and (screen models) radiator fan speeds.
    m.kraken = ctl.kraken();
    if (m.kraken.valid) {
        m.pumpRpm = m.kraken.pumpRpm;
        if (m.kraken.fanRpm > 0 && m.layout.cooler == pc::Cooler::Aio)
            for (int i = 0; i < pc::kSlots; ++i)
                if (pc::Slots()[static_cast<size_t>(i)].mount == m.layout.radiator && m.layout.slots[static_cast<size_t>(i)] != pc::SlotFan::None)
                    m.fanRpm[static_cast<size_t>(i)] = m.kraken.fanRpm;
    }
    for (const auto& d : ctl.openRgb().devices())
        if (d.type == 2 && ctl.OpenRgbOn(d)) m.gpuRgb = true;  // a graphics card OpenRGB lights

    // The desk: the keyboard and mouse LumaBridge lights, else plain ones; a headset if one
    // was found; the other lit devices as light bars.
    m.keyboard = {kKeyboard, "Keyboard", nullptr, "desk:keyboard"};
    m.mouse = {kMouse, "Mouse", nullptr, "desk:mouse"};
    if (p.azothKeyboard && HasAzoth(ctl)) m.keyboard = {kKeyboard, "ASUS ROG Azoth", device::kKeyboard, "desk:keyboard"};
    if (p.dualsenseController && HasDualSense(ctl)) {
        m.controller = true;
        m.controllerGear = {kController, "DualSense", device::kController, "desk:controller", 1};
    }
    if (p.logitechDevices && !DeviceNative(p, device::kMouse))
        for (const auto& d : LogitechRgb(ctl)) {
            const std::string name = "Logitech " + d.name;
            if (d.type == 0 && !m.keyboard.device) m.keyboard = {kKeyboard, name, device::kMouse, "desk:keyboard"};
            else if (d.type == 3 && !m.mouse.device) m.mouse = {kMouse, name, device::kMouse, "desk:mouse"};
            else if (d.type == 8 && !m.headset) {
                m.headset = true;
                m.headsetGear = {kHeadset, name, device::kMouse, "desk:headset"};
            }
        }
    auto other = [&](const std::string& name, int kind, uint32_t leds) {
        if (kind == 1 && !m.keyboard.device) m.keyboard = {kKeyboard, name, device::kOther, "desk:keyboard"};
        else if (kind == 2 && !m.mouse.device) m.mouse = {kMouse, name, device::kOther, "desk:mouse"};
        else if (kind == 8 && !m.headset) {
            m.headset = true;
            m.headsetGear = {kHeadset, name, device::kOther, "desk:headset"};
        } else if (static_cast<int>(m.others.size()) < pc::kMaxOthers) {
            const int i = static_cast<int>(m.others.size());
            m.others.push_back({kOther0 + i, name, device::kOther, "desk:other" + std::to_string(i),
                                static_cast<int>(std::clamp<uint32_t>(leds, 4, 24))});
        }
    };
    for (const auto& d : LampArrayLit(ctl)) other(d.name, d.kind == 1 ? 1 : d.kind == 2 ? 2 : 0, d.lamps);
    for (const auto& d : OpenRgbLit(ctl)) {
        if (d.type == 2 || d.type == 0 || d.type == 1) continue;  // the graphics card, board and memory are in the case
        other(d.name, d.type == 5 ? 1 : d.type == 6 ? 2 : d.type == 8 ? 8 : 0, d.leds);
    }
    return m;
}

s3d::V3 DeskSpot(const Prefs& p, const std::string& item) {
    auto it = p.setupSpots.find(item);
    return it == p.setupSpots.end() ? pc::DefaultDeskSpot(item) : pc::FromSaved(it->second.x, it->second.y);
}

// How far something is turned on the desk (radians, counter-clockwise seen from above).
float DeskAngle(const Prefs& p, const std::string& item) {
    auto it = p.setupSpots.find(item);
    return it == p.setupSpots.end() ? 0.f : it->second.angle * 0.01745329f;
}

// Where a desk item stands, turned as it is.
s3d::Transform DeskXf(const Prefs& p, const std::string& item) { return s3d::Transform::YawAt(DeskAngle(p, item), DeskSpot(p, item)); }

// Where the case stands and how it's turned (turn 0: its front towards you). Mirrored, as a
// real case is: seen from the front, the glass is on the left and the motherboard on the right.
s3d::Transform CaseXf(const Prefs& p, const pc::Layout& l) {
    return s3d::Transform::YawAt(-1.5707963f * static_cast<float>(l.turn) + DeskAngle(p, "desk:case"),
                                 DeskSpot(p, "desk:case") + s3d::V3{0, 1.2f, 0})
        .MirroredX();
}

// Something to point at: its name at a spot (world), and more on hover.
struct Anchor {
    int obj;
    s3d::V3 at;
    std::string label;
};

// Text shown on a surface in the scene (the pump's screen): where, which way the surface faces,
// which way is up on it, how tall the text is (cm), and its color.
struct SurfaceText {
    s3d::V3 at, normal, up;
    float height;
    std::string text;
    uint32_t color;
};

// ---- Parts ----------------------------------------------------------------------------------

// A fan in the current placement's xy plane (facing z): frame, LED ring (empty: none), blades.
void Fan(s3d::Scene& sc, float r, const std::vector<Rgb>& leds, float angle, double rpm, int id, bool frame = true) {
    using s3d::V3;
    const uint32_t frameCol = H(0x333946), bladeCol = H(0x6A7384, rpm > 1200 ? 150 : 235);
    if (frame) {
        const float h = r + 0.5f, w = 0.8f, d = 1.2f;
        sc.Box({-h, -h, -d}, {h, -h + w, d}, frameCol, id);
        sc.Box({-h, h - w, -d}, {h, h, d}, frameCol, id);
        sc.Box({-h, -h + w, -d}, {-h + w, h - w, d}, frameCol, id);
        sc.Box({h - w, -h + w, -d}, {h, h - w, d}, frameCol, id);
    }
    // LED ring: the fan's LEDs around it (at most 24 drawn), on both faces.
    const int n = leds.empty() ? 20 : std::min<int>(24, static_cast<int>(leds.size()));
    for (int i = 0; i < n; ++i) {
        const float a0 = 6.2831853f * static_cast<float>(i) / static_cast<float>(n), a1 = 6.2831853f * static_cast<float>(i + 1) / static_cast<float>(n);
        const float r0 = r * 0.9f, r1 = r * 0.98f;
        const Rgb c = leds.empty() ? Rgb{78, 84, 98} : leds[static_cast<size_t>(i * static_cast<int>(leds.size()) / n)];
        const uint32_t col = C(c);
        sc.Quad({std::cos(a0) * r0, std::sin(a0) * r0, 0.9f}, {std::cos(a1) * r0, std::sin(a1) * r0, 0.9f},
                {std::cos(a1) * r1, std::sin(a1) * r1, 0.9f}, {std::cos(a0) * r1, std::sin(a0) * r1, 0.9f}, col, id,
                s3d::kDoubleSided | (leds.empty() ? 0u : static_cast<uint32_t>(s3d::kEmissive)));
        if (!leds.empty() && i % 3 == 0) sc.AddGlow({std::cos(a0) * r * 0.94f, std::sin(a0) * r * 0.94f, 1.0f}, r * 0.2f, C(c, 55));
    }
    // Blades, a blur disc when they're fast, and the hub.
    if (rpm > 1200) sc.Disc(r * 0.86f, 0.05f, 8, H(0x5A6070, 40), id, s3d::kDoubleSided);
    for (int b = 0; b < 7; ++b) {
        const float a = angle + 6.2831853f * static_cast<float>(b) / 7.f;
        const float r0 = r * 0.28f, r1 = r * 0.86f;
        sc.Quad({std::cos(a) * r0, std::sin(a) * r0, 0}, {std::cos(a + 0.22f) * r1, std::sin(a + 0.22f) * r1, 0.2f},
                {std::cos(a + 0.62f) * r1, std::sin(a + 0.62f) * r1, 0.2f}, {std::cos(a + 0.42f) * r0, std::sin(a + 0.42f) * r0, 0},
                bladeCol, id, s3d::kDoubleSided);
    }
    sc.Disc(r * 0.3f, 0.3f, 8, H(0x444B58), id, s3d::kDoubleSided);
}

// LED bar along x from x0 to x1 at (y, z), facing +z, with glows.
void LedBar(s3d::Scene& sc, float x0, float x1, float y0, float y1, float z, const std::vector<Rgb>& leds, int id) {
    const int n = static_cast<int>(leds.size());
    for (int i = 0; i < n; ++i) {
        const float a = x0 + (x1 - x0) * static_cast<float>(i) / static_cast<float>(n), b = x0 + (x1 - x0) * static_cast<float>(i + 1) / static_cast<float>(n);
        sc.Quad({a, y0, z}, {b, y0, z}, {b, y1, z}, {a, y1, z}, C(leds[static_cast<size_t>(i)]), id, s3d::kEmissive);
        if (i % 2 == 0) sc.AddGlow({(a + b) / 2, (y0 + y1) / 2, z}, (y1 - y0) * 2.2f, C(leds[static_cast<size_t>(i)], 60));
    }
}

// Closed round tubes keep their shape when viewed from either side.
void Tube(s3d::Scene& sc, s3d::V3 a, s3d::V3 b, float r, uint32_t color, int id) {
    model3d::Tube(sc, a, b, r, color, id);
}

// Live colors of `n` LEDs of a device (grey while LumaBridge isn't lighting it).
std::vector<Rgb> Leds(Controller& ctl, const char* device, double t, int n, bool one = false) {
    std::vector<Rgb> out(static_cast<size_t>(n), kDark);
    if (!device) return out;
    const fx::Params* p = LiveParams(ctl, device);
    const double level = LiveLevel(ctl, device);
    for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = LiveAt(p, t, one ? 0 : i, one ? 1 : n, level);
    return out;
}

struct Spin {
    std::array<float, 16> angle{};
    double at = -1;
    double wind = 0;        // how far the airflow has moved (cycles), summed frame by frame
    double windSpeed = -1;  // its speed, eased towards the fans' so it never jumps
};

// ---- The scene ------------------------------------------------------------------------------

struct Options {
    bool desk = true;        // the desk and what's on it (else the case alone)
    bool editSlots = false;  // show the empty fan slots, to click
    bool airflow = false;
};

void Build(s3d::Scene& sc, Controller& ctl, const Model& m, const Options& o, double t, Spin& spin,
           std::vector<Anchor>* anchors, std::vector<SurfaceText>* texts = nullptr) {
    using s3d::V3;
    const Prefs& p = ctl.prefs();
    // Spin: each fan turns at its speed (scaled: 1000 RPM is one turn a second, so the blades
    // stay readable; the tooltip has the real number).
    const double dt = spin.at < 0 ? 0 : std::min(0.1, t - spin.at);
    spin.at = t;
    auto turn = [&](int k, double rpm) {
        spin.angle[static_cast<size_t>(k)] += static_cast<float>(dt * (rpm < 0 ? 800 : rpm) / 1000.0 * 6.2831853);
        return spin.angle[static_cast<size_t>(k)];
    };

    // The desk.
    if (o.desk) {
        sc.xf = {};
        sc.Box({-pc::kDeskW / 2, -3, -pc::kDeskD / 2}, {pc::kDeskW / 2, 0, pc::kDeskD / 2}, H(0x3A3530), kDesk, s3d::kBackground);
    }

    // ---- The case ----
    const s3d::Transform cx = o.desk ? CaseXf(p, m.layout) : s3d::Transform{}.MirroredX();
    sc.xf = cx;
    const float W = pc::kCaseW / 2, D = pc::kCaseD / 2, Hh = pc::kCaseH;
    sc.Room({-W, 0, -D}, {W, Hh, D}, H(0x262C37), kCase);
    pc3d::Chassis(sc, kCase);
    for (float footX : {-W + 2, W - 2})  // feet
        for (float footZ : {-D + 3, D - 3})
            sc.Box({footX - 1.5f, -1.2f, footZ - 1.5f}, {footX + 1.5f, 0, footZ + 1.5f}, H(0x0E1015), kCase);
    sc.Edges({-W, 0, -D}, {W, Hh, D}, H(0x8A93A8, 90), 1.2f);
    // Glass: a faint sheen on the glass side's edges.
    sc.AddLine({W, 0.5f, -D + 0.5f}, {W, Hh - 0.5f, -D + 0.5f}, H(0xBFD4FF, 60), 2);
    anchors->push_back({kCase, cx.Apply({0, Hh + 3, 0}), "Case"});

    // PSU shroud (with the power supply under it).
    sc.bias = 4;  // what sits on it sorts in front
    sc.Box({-W + 0.2f, 0, -D + 0.2f}, {W - 0.2f, pc::kShroudH, D - 0.2f}, H(0x323845), kPsu);
    sc.bias = 0;
    sc.Box({-W + 3, 2, -D + 0.6f}, {-W + 17, 8.6f, -D + 15}, H(0x22272F), kPsu);
    anchors->push_back({kPsu, cx.Apply({W, 5, 10}), "Power supply"});

    // Motherboard, VRM and I/O covers, chipset; its RGB on the I/O cover.
    const float bx = -W + 0.6f;
    sc.bias = 6;
    sc.Box({bx, 14, -21}, {bx + 0.4f, 44, 3.5f}, H(0x2B3547), kBoard);
    sc.bias = 0;
    sc.Box({bx + 0.4f, 40, -18}, {bx + 2.2f, 43.5f, -8}, H(0x3A414F), kBoard);
    sc.Box({bx + 0.4f, 31, -21}, {bx + 2.8f, 43.5f, -18.3f}, H(0x3C4452), kBoard);
    sc.Box({bx + 0.4f, 15, -3}, {bx + 1.6f, 19.5f, 2.5f}, H(0x3A414F), kBoard);
    {
        const std::vector<Rgb> leds = m.boardRgb ? Leds(ctl, device::kBoard, t, std::clamp(m.boardLeds, 3, 12))
                                                 : std::vector<Rgb>(6, Rgb{70, 76, 90});
        const int n = static_cast<int>(leds.size());
        for (int i = 0; i < n; ++i) {
            const float y0 = 32 + 10.5f * static_cast<float>(i) / static_cast<float>(n), y1 = 32 + 10.5f * static_cast<float>(i + 1) / static_cast<float>(n);
            sc.Quad({bx + 2.85f, y0, -18.3f}, {bx + 2.85f, y0, -20.8f}, {bx + 2.85f, y1, -20.8f}, {bx + 2.85f, y1, -18.3f},
                    C(leds[static_cast<size_t>(i)]), kBoard, m.boardRgb ? s3d::kEmissive : 0u);
            if (m.boardRgb && i % 2 == 0) sc.AddGlow({bx + 3.2f, (y0 + y1) / 2, -19.5f}, 1.6f, C(leds[static_cast<size_t>(i)], 70));
        }
    }
    anchors->push_back({kBoard, cx.Apply({bx, 45.5f, -9}), m.boardName.empty() ? std::string("Motherboard") : m.boardName});

    pc3d::BoardDetails(sc, kBoard);

    // Storage: M.2 drives on the board, more as SSDs on the shroud.
    for (int i = 0; i < std::min(m.drives, 2); ++i) {
        const float y = i == 0 ? 29.4f : 18.2f;
        sc.Box({bx + 0.4f, y, -18}, {bx + 1.0f, y + 1.2f, -9.5f}, H(0x4A5262), kStorage);
    }
    if (m.drives > 2) sc.Box({1, pc::kShroudH, -19}, {9, pc::kShroudH + 0.8f, -9}, H(0x3A414F), kStorage);
    if (m.drives) anchors->push_back({kStorage, cx.Apply({bx + 1, 29.4f, -9}), m.drives == 1 ? "Drive" : std::to_string(m.drives) + " drives"});

    // CPU cooler (no RGB LumaBridge controls): a tower heatsink with its fan in front, or an AIO:
    // the pump on the CPU, two tubes, and the radiator behind the top or front fans.
    if (m.layout.cooler == pc::Cooler::Air) {
        // Individual fins and heat pipes stay separated when the camera turns.
        for (int i = 0; i < 15; ++i) {
            const float y = 31.2f + static_cast<float>(i) * 0.82f;
            sc.Box({bx + 0.4f, y, -12.8f}, {3, y + 0.3f, -7.2f}, H(0x9AA2AE), kCpu);
        }
        for (float x : {-7.f, -3.f, 1.f}) {
            sc.xf = s3d::Transform::Facing({0, 1, 0}, {x, 31, -10}).Then(cx);
            model3d::Cylinder(sc, 0.3f, 0, 13.5f, H(0xC0B0A0), kCpu, 10);
        }
        sc.xf = cx;
        sc.Box({bx + 0.4f, 43.6f, -12.8f}, {3, 44.4f, -7.2f}, H(0x2A2F38), kCpu);
        sc.xf = s3d::Transform::Facing({0, 0, 1}, {-3.2f, 37.3f, -6.2f}).Then(cx);
        Fan(sc, 5.6f, {}, turn(9, m.cpuFanRpm), m.cpuFanRpm, kCpu);
        sc.xf = cx;
    } else {
        sc.Box({bx + 0.4f, 34, -13}, {bx + 3.6f, 40, -7}, H(0x2A2F38), kCpu);
        sc.xf = s3d::Transform::Facing({1, 0, 0}, {bx + 3.65f, 37, -10}).Then(cx);
        sc.Disc(2.7f, 0, 8, H(0x4A5262), kCpu);
        if (m.aio && m.aio->lcd) {
            // The pump's screen, as NZXT's shows it by default: the liquid's temperature (as the
            // Kraken reports it), else the CPU's, as a ring.
            sc.Disc(2.45f, 0.05f, 8, H(0x0A0F1A), kCpu, s3d::kEmissive);
            const double shown = m.kraken.valid ? m.kraken.liquidC : m.cpuTemp;
            const float frac = shown < 0 ? 0.35f : static_cast<float>(std::clamp((m.kraken.valid ? (shown - 20) * 2 : shown) / 100.0, 0.05, 1.0));
            const uint32_t arc = shown < 0 ? H(0x5A6272) : Hex(TempColor(m.kraken.valid ? shown + 20 : shown));
            const int segs = std::max(1, static_cast<int>(24 * frac));
            for (int i = 0; i < segs; ++i) {
                const float a0 = 1.5707963f - 6.2831853f * static_cast<float>(i) / 24.f, a1 = 1.5707963f - 6.2831853f * static_cast<float>(i + 1) / 24.f;
                sc.Quad({std::cos(a0) * 1.55f, std::sin(a0) * 1.55f, 0.1f}, {std::cos(a1) * 1.55f, std::sin(a1) * 1.55f, 0.1f},
                        {std::cos(a1) * 2.05f, std::sin(a1) * 2.05f, 0.1f}, {std::cos(a0) * 2.05f, std::sin(a0) * 2.05f, 0.1f}, arc, kCpu,
                        s3d::kEmissive | s3d::kDoubleSided);
            }
            sc.AddGlow({0, 0, 0.3f}, 2.4f, s3d::WithAlpha(arc, 40));
            // ...and the number in the middle.
            if (texts && shown >= 0) {
                char num[16];
                snprintf(num, sizeof num, "%.0f\xC2\xB0", shown);
                const V3 c = sc.xf.Apply({0, 0, 0.12f});
                texts->push_back({c, sc.xf.Apply({0, 0, 1}) - sc.xf.Apply({0, 0, 0}), sc.xf.Apply({0, 1, 0}) - sc.xf.Apply({0, 0, 0}), 1.5f,
                                  num, Hex(0xF2F5FA)});
            }
        } else {
            sc.Disc(1.6f, 0.05f, 8, H(0x5E6778), kCpu);
        }
        if (m.layout.pumpRgb) {
            // The pump head's light ring, chained on the ARGB header: the fans' lighting.
            const std::vector<Rgb> ring = Leds(ctl, m.fansRgb ? device::kFans : nullptr, t, 12);
            const bool lit = m.fansRgb && LiveParams(ctl, device::kFans);
            for (int i = 0; i < 12; ++i) {
                const float a0 = 6.2831853f * static_cast<float>(i) / 12.f, a1 = 6.2831853f * static_cast<float>(i + 1) / 12.f;
                sc.Quad({std::cos(a0) * 2.2f, std::sin(a0) * 2.2f, 0.1f}, {std::cos(a0) * 2.75f, std::sin(a0) * 2.75f, 0.1f},
                        {std::cos(a1) * 2.75f, std::sin(a1) * 2.75f, 0.1f}, {std::cos(a1) * 2.2f, std::sin(a1) * 2.2f, 0.1f},
                        C(ring[static_cast<size_t>(i)]), kCpu, s3d::kDoubleSided | (lit ? static_cast<uint32_t>(s3d::kEmissive) : 0u));
                if (lit && i % 3 == 0) sc.AddGlow({std::cos(a0) * 2.5f, std::sin(a0) * 2.5f, 0.3f}, 1.2f, C(ring[static_cast<size_t>(i)], 70));
            }
        }
        sc.xf = cx;
        // The radiator, over the fans at its place (at least two fans long).
        const bool top = m.layout.radiator == pc::Mount::Top;
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < pc::kSlots; ++i) {
            const pc::Slot& sl = pc::Slots()[static_cast<size_t>(i)];
            if (sl.mount != m.layout.radiator || m.layout.slots[static_cast<size_t>(i)] == pc::SlotFan::None) continue;
            const float c = top ? sl.center.z : sl.center.y;
            lo = std::min(lo, c);
            hi = std::max(hi, c);
        }
        if (lo > hi) lo = hi = top ? 6.5f : 34.5f;
        if (hi - lo < 12) {  // two fans long, inside the case
            const float first = top ? -11.5f : 16.5f;
            lo = std::max(first, hi - 12);
            hi = lo + 12;
        }
        // Blowing out, the radiator sits against the panel with its fans inside pulling air
        // from the case through it; pulling in, the fans sit at the panel and push through it.
        const bool out = m.layout.exhaust[static_cast<size_t>(m.layout.radiator)];
        V3 rlo, rhi, port;
        if (top) {
            rlo = {-4.4f, out ? 43.9f : 41.2f, lo - 6.6f};
            rhi = {8.4f, out ? 46.6f : 43.8f, hi + 6.6f};
            port = {2, rlo.y, rlo.z + 2};
        } else {
            rlo = {-6.6f, std::max(pc::kShroudH + 0.5f, lo - 6.6f), out ? 19.5f : 17.1f};
            rhi = {6.6f, hi + 6.6f, out ? 22.2f : 19.6f};
            port = {0, rhi.y - 2, rlo.z};
        }
        sc.bias = 2;
        sc.Box(rlo, rhi, H(0x2C323C), kCpu);
        sc.bias = 0;
        for (int k = 1; k < 12; ++k) {  // fins
            const float f = static_cast<float>(k) / 12.f;
            if (top) sc.AddLine({rlo.x + 0.2f, rlo.y - 0.02f, rlo.z + (rhi.z - rlo.z) * f}, {rhi.x - 0.2f, rlo.y - 0.02f, rlo.z + (rhi.z - rlo.z) * f}, H(0x5A6272, 160));
            else sc.AddLine({rlo.x + 0.2f, rlo.y + (rhi.y - rlo.y) * f, rlo.z - 0.02f}, {rhi.x - 0.2f, rlo.y + (rhi.y - rlo.y) * f, rlo.z - 0.02f}, H(0x5A6272, 160));
        }
        // Tubes: out of the pump's side, up and over to the radiator's end.
        for (int k = 0; k < 2; ++k) {
            const float d = k ? 1.1f : -1.1f;
            const V3 a{bx + 3.2f, 39.5f, -10 + d}, bend{bx + 5.5f, 41.f, -10 + d};
            const V3 end = top ? V3{port.x + d, port.y, port.z} : V3{port.x, port.y + d, port.z};
            Tube(sc, a, bend, 0.5f, H(0x3A414E), kCpu);
            Tube(sc, bend, end, 0.5f, H(0x3A414E), kCpu);
        }
    }
    anchors->push_back({kCpu, cx.Apply({-3, 45, -10}), m.cpuName.empty() ? std::string("Processor") : m.cpuName});

    // Memory: the sticks in their slots, their light bars along the top edge.
    {
        const std::vector<Rgb> leds = m.ramRgb ? Leds(ctl, device::kRam, t, 20) : std::vector<Rgb>(20, Rgb{70, 76, 90});
        int shown = 0;
        for (int s = 0; s < 4; ++s) {
            if (!m.ram[static_cast<size_t>(s)]) continue;
            const float z = -3.6f + 0.95f * static_cast<float>(s);
            sc.Box({bx + 0.4f, 27.5f, z - 0.35f}, {bx + 4.6f, 41.5f, z + 0.35f}, H(0x3A4152), kRam);
            for (int i = 0; i < 5; ++i) {
                const Rgb c = leds[static_cast<size_t>(s * 5 + i)];
                const float y0 = 28 + 2.7f * static_cast<float>(i), y1 = y0 + 2.7f;
                sc.Box({bx + 4.6f, y0, z - 0.3f}, {bx + 5.3f, y1, z + 0.3f}, C(c), kRam, m.ramRgb ? s3d::kEmissive : 0u);
                if (m.ramRgb && i % 2 == 0) sc.AddGlow({bx + 5.6f, (y0 + y1) / 2, z}, 1.3f, C(c, 60));
            }
            ++shown;
        }
        if (shown) anchors->push_back({kRam, cx.Apply({bx + 5, 42, -2.2f}), m.ramName.empty() ? std::string("Memory") : m.ramName});
    }

    // Graphics card: shroud, backplate, three fans underneath, and a light strip on its edge.
    if (!m.gpuName.empty()) {
        sc.bias = 3;
        sc.Box({bx + 0.4f, 20.5f, -21}, {3.6f, 26, 10}, H(0x3E4553), kGpu);
        sc.bias = 0;
        sc.Box({bx + 0.4f, 26, -21}, {3.6f, 26.4f, 10}, H(0x565E6E), kGpu);
        sc.Box({bx + 0.4f, 17, -21.8f}, {3.6f, 26.4f, -21}, H(0x6A7282), kGpu);  // bracket
        pc3d::GpuDetails(sc, kGpu);
        const std::vector<Rgb> strip = m.gpuRgb ? Leds(ctl, device::kOther, t, 10) : std::vector<Rgb>(10, Rgb{58, 63, 74});
        for (int i = 0; i < 10; ++i) {
            const float z0 = -15 + 2.f * static_cast<float>(i), z1 = z0 + 2.f;
            sc.Quad({4.12f, 22.6f, z1}, {4.12f, 22.6f, z0}, {4.12f, 23.8f, z0}, {4.12f, 23.8f, z1}, C(strip[static_cast<size_t>(i)]), kGpu,
                    m.gpuRgb ? s3d::kEmissive : 0u);
        }
        const double gpuRpm = m.gpuFanPct < 0 ? -1 : m.gpuFanPct * 30;
        for (int f = 0; f < 3; ++f) {
            sc.xf = s3d::Transform::Facing({0, -1, 0}, {-3.4f, 20.4f, -14.8f + 9.2f * static_cast<float>(f)}).Then(cx);
            Fan(sc, 4.3f, {}, turn(10 + f, gpuRpm), gpuRpm, kGpu, false);
        }
        sc.xf = cx;
        anchors->push_back({kGpu, cx.Apply({3.6f, 27.5f, -5}), m.gpuName});
    }

    // Case fans: in their slots; RGB fans show the ARGB header's LEDs in chain order.
    {
        const fx::FanLayout& fl = ctl.config().argbFans;
        std::vector<Rgb> all;
        const fx::Params* fp = m.fansRgb ? LiveParams(ctl, device::kFans) : nullptr;
        if (fp) {
            fx::RenderFans(*fp, t, fl, &all);
            const double level = LiveLevel(ctl, device::kFans);
            for (Rgb& c : all) c = Scale(c, level);
        }
        const int per = fl.LedsPerFan();
        for (int i = 0; i < pc::kSlots; ++i) {
            const pc::Slot& s = pc::Slots()[static_cast<size_t>(i)];
            const pc::SlotFan f = m.layout.slots[static_cast<size_t>(i)];
            // Behind a radiator that blows out, the fans move inside it (see the radiator).
            const bool behindRadiator = m.layout.cooler == pc::Cooler::Aio && s.mount == m.layout.radiator &&
                                        m.layout.exhaust[static_cast<size_t>(s.mount)];
            sc.xf = s3d::Transform::Facing(s.inward, s.center + s.inward * (behindRadiator ? 3.4f : 0.f)).Then(cx);
            if (f == pc::SlotFan::None) {
                if (o.editSlots) {
                    // An empty slot: a faint square to click.
                    sc.Quad({-s.radius, -s.radius, 0}, {s.radius, -s.radius, 0}, {s.radius, s.radius, 0}, {-s.radius, s.radius, 0},
                            H(0x7C6CFF, 45), kFan0 + i, s3d::kDoubleSided);
                    for (int k = 0; k < 16; k += 2) {
                        const float a0 = 6.2831853f * static_cast<float>(k) / 16.f, a1 = 6.2831853f * static_cast<float>(k + 1) / 16.f;
                        sc.AddLine({std::cos(a0) * s.radius, std::sin(a0) * s.radius, 0}, {std::cos(a1) * s.radius, std::sin(a1) * s.radius, 0},
                                   H(0x9384FF, 200), 1.5f);
                    }
                }
                continue;
            }
            std::vector<Rgb> leds;
            if (f == pc::SlotFan::Rgb) {
                const int k = m.layout.ChainIndex(i);
                leds.assign(static_cast<size_t>(per), fp ? Rgb{} : kDark);
                for (int j = 0; j < per && fp && !all.empty(); ++j) leds[static_cast<size_t>(j)] = all[static_cast<size_t>((k * per + j) % static_cast<int>(all.size()))];
            }
            const double rpm = m.fanRpm[static_cast<size_t>(i)];
            Fan(sc, s.radius, leds, turn(i, rpm) * (m.layout.Exhaust(i) ? -1.f : 1.f), rpm, kFan0 + i);
            anchors->push_back({kFan0 + i, sc.xf.Apply({0, 0, 0}), std::string(pc::MountName(s.mount)) + " fan"});
        }
        sc.xf = cx;
    }

    // Airflow: wind streaks from the intakes through the case to the exhausts, cool to warm.
    if (o.airflow && m.layout.Fans()) {
        double sum = 0;
        int n = 0;
        for (double r : m.fanRpm)
            if (r > 0) sum += r, ++n;
        // The streaks move on by what the fans did this frame (eased), so a new fan reading
        // changes their speed, never their place.
        const double target = (n ? sum / n : 900) / 1000.0 * 0.3;
        spin.windSpeed = spin.windSpeed < 0 ? target : spin.windSpeed + (target - spin.windSpeed) * std::min(1.0, dt * 1.5);
        spin.wind += dt * spin.windSpeed;
        const Rgb cool{90, 180, 255}, warm{255, 150, 80};
        auto tint = [&](float k) {
            return Rgb{static_cast<uint8_t>(cool.r + (warm.r - cool.r) * k), static_cast<uint8_t>(cool.g + (warm.g - cool.g) * k),
                       static_cast<uint8_t>(cool.b + (warm.b - cool.b) * k)};
        };
        auto smooth = [](float e0, float e1, float x) {
            const float k = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
            return k * k * (3 - 2 * k);
        };
        constexpr int kStreaks = 40, kSegs = 8;
        for (int i = 0; i < kStreaks; ++i) {
            // Each streak runs its path once, then starts on a new one (another intake, another
            // exhaust, another spot on the fans), faded out at both ends so none pops.
            const double life = 1.0 + 0.35 * static_cast<double>((static_cast<uint32_t>(i) * 2246822519u) % 1000u) / 1000.0;
            const double local = spin.wind / life + static_cast<double>((static_cast<uint32_t>(i) * 2654435761u) % 1000u) / 1000.0;
            const uint32_t cycle = static_cast<uint32_t>(static_cast<int64_t>(std::floor(local)));
            const uint32_t seed = static_cast<uint32_t>(i) * 7919u + cycle * 104729u;
            const float len = 0.14f + 0.08f * static_cast<float>((seed * 40503u >> 8) % 100u) / 100.f;
            const float head = static_cast<float>(local - std::floor(local)) * (1.f + len);
            const int intake = static_cast<int>((seed * 2654435761u) >> 20), exhaust = static_cast<int>((seed * 2246822519u) >> 20);
            const float env = smooth(0.f, 0.18f, head) * smooth(0.f, 0.18f, 1.f + len - head);
            if (env <= 0.01f) continue;
            V3 prev{};
            bool have = false;
            for (int k = 0; k <= kSegs; ++k) {
                const float ph = head - len * static_cast<float>(k) / kSegs;
                if (ph > 1.f) continue;
                if (ph < 0.f) break;
                const V3 at = pc::AirflowPoint(m.layout, intake, exhaust, ph, seed);
                if (have) {
                    const float fade = 1.f - static_cast<float>(k - 1) / kSegs;
                    sc.AddLine(prev, at, C(tint(ph), static_cast<int>(160 * fade * env)), 1.2f + 0.8f * fade);
                }
                prev = at;
                have = true;
            }
        }
    }

    if (!o.desk) return;

    // ---- On the desk ----
    // Monitors, as Windows has them arranged (one, if it doesn't say).
    {
        std::vector<displays::Display> screens = m.screens;
        if (screens.empty()) screens.push_back({});
        const std::vector<displays::Placed> placed = displays::Arrange(screens);
        const s3d::Transform group = DeskXf(p, "desk:monitor");
        for (size_t i = 0; i < placed.size(); ++i) {
            const displays::Placed& d = placed[i];
            const int id = kMonitor0 + static_cast<int>(i);
            sc.xf = s3d::Transform::YawAt(d.yaw, {d.x, 0, d.z}).Then(group);
            if (d.resting) {
                // A small screen with no stand, leaning back on the desk (or on the stand of the one above).
                s3d::Transform lean;
                const float a = d.lean;
                lean.y = {0, std::cos(a), -std::sin(a)};
                lean.z = {0, std::sin(a), std::cos(a)};
                sc.xf = lean.Then(sc.xf);
            }
            const float hw = d.w / 2 + 0.8f, top = d.bottom + d.h + 0.8f;
            monitor3d::Support(sc, d, id);
            sc.Box({-hw, std::max(0.f, d.bottom - 0.8f), -4.4f}, {hw, top, -2.5f}, H(0x23272F), id);
            sc.Quad({-d.w / 2, d.bottom, -2.45f}, {d.w / 2, d.bottom, -2.45f}, {d.w / 2, d.bottom + d.h, -2.45f},
                    {-d.w / 2, d.bottom + d.h, -2.45f}, H(0x0D1422), id, s3d::kEmissive);
            if (screens[i].primary)  // the taskbar tells the primary display apart
                sc.Quad({-d.w / 2, d.bottom, -2.44f}, {d.w / 2, d.bottom, -2.44f}, {d.w / 2, d.bottom + 1.2f, -2.44f},
                        {-d.w / 2, d.bottom + 1.2f, -2.44f}, H(0x1C2A44), id, s3d::kEmissive);
            anchors->push_back({id, sc.xf.Apply({0, top + 2, -3}), m.screens.empty() ? std::string("Monitor") : screens[i].name});
        }
    }
    // Sculpted keyboard and mouse, with the same per-LED colors as the actual devices.
    {
        const Gear& g = m.keyboard;
        sc.xf = DeskXf(p, g.spot);
        const fx::Params* kp = g.device ? LiveParams(ctl, g.device) : nullptr;
        const double level = g.device ? LiveLevel(ctl, g.device) : 0;
        std::vector<Rgb> colors;
        if (kp) colors = azoth::RenderKeys(*kp, t, level);
        desk3d::Keyboard(sc, kKeyboard, colors, kp != nullptr, Is(g.device, device::kKeyboard));
        anchors->push_back({kKeyboard, sc.xf.Apply({0, 5, -8}), g.name});
    }
    {
        const Gear& g = m.mouse;
        sc.xf = DeskXf(p, g.spot);
        const bool perLed = Is(g.device, device::kMouse) && ctl.logitech().mouseEffect();
        const std::vector<Rgb> leds = Leds(ctl, g.device, t, 8, !perLed);
        desk3d::Mouse(sc, kMouse, leds, g.device && LiveParams(ctl, g.device));
        anchors->push_back({kMouse, sc.xf.Apply({0, 7, 0}), g.name});
    }
    // Headset on its stand.
    if (m.headset) {
        const Gear& g = m.headsetGear;
        const s3d::Transform at = DeskXf(p, g.spot);
        sc.xf = at;
        sc.bias = 3;
        sc.Box({-5, 0, -5}, {5, 1, 5}, H(0x23272F), kHeadset);
        sc.bias = 0;
        sc.Box({-0.8f, 1, -0.8f}, {0.8f, 24, 0.8f}, H(0x2B3039), kHeadset);
        sc.Box({-9, 24, -2}, {9, 26, 2}, H(0x1E222A), kHeadset);
        const std::vector<Rgb> leds = Leds(ctl, g.device, t, 2);
        const bool lit = g.device && LiveParams(ctl, g.device);
        for (int side = 0; side < 2; ++side) {
            const float x = side ? 8.5f : -8.5f;
            sc.Box({x - 1.6f, 13, -4}, {x + 1.6f, 23, 4}, H(0x1B1F27), kHeadset);
            const float fx = side ? x + 1.62f : x - 1.62f;
            sc.xf = s3d::Transform::Facing({side ? 1.f : -1.f, 0, 0}, {fx, 18, 0}).Then(at);
            sc.Disc(2.6f, 0, 8, C(leds[static_cast<size_t>(side)]), kHeadset, lit ? s3d::kEmissive : 0u);
            if (lit) sc.AddGlow({0, 0, 0.2f}, 3.4f, C(leds[static_cast<size_t>(side)], 70));
            sc.xf = at;
        }
        anchors->push_back({kHeadset, sc.xf.Apply({0, 29, 0}), g.name});
    }
    // DualSense, with the live lightbar beside its touchpad.
    if (m.controller) {
        const Gear& g = m.controllerGear;
        sc.xf = DeskXf(p, g.spot);
        sc.bias = 0;
        const std::vector<Rgb> leds = Leds(ctl, g.device, t, 1);
        controller3d::Build(sc, kController, C(leds[0]), g.device && LiveParams(ctl, g.device));
        anchors->push_back({kController, sc.xf.Apply({0, 6, -2}), g.name});
    }
    // Other lit devices: light bars.
    for (const Gear& g : m.others) {
        sc.xf = DeskXf(p, g.spot);
        sc.bias = 2;
        sc.Box({-10, 0, -1.5f}, {10, 2.4f, 1.5f}, H(0x1B1F27), g.obj);
        sc.bias = 0;
        LedBar(sc, -9.5f, 9.5f, 0.6f, 2.0f, 1.52f, Leds(ctl, g.device, t, g.leds), g.obj);
        anchors->push_back({g.obj, sc.xf.Apply({0, 5, 0}), g.name});
    }
    sc.xf = {};
}

// The device whose lighting a part shows (nullptr: none LumaBridge controls).
const char* DeviceOf(const Model& m, int obj) {
    if (obj >= kFan0 && obj < kFan0 + pc::kSlots)
        return m.fansRgb && m.layout.slots[static_cast<size_t>(obj - kFan0)] == pc::SlotFan::Rgb ? device::kFans : nullptr;
    switch (obj) {
    case kCpu: return m.layout.cooler == pc::Cooler::Aio && m.layout.pumpRgb && m.fansRgb ? device::kFans : nullptr;
    case kBoard: return m.boardRgb ? device::kBoard : nullptr;
    case kRam: return m.ramRgb ? device::kRam : nullptr;
    case kGpu: return m.gpuRgb ? device::kOther : nullptr;
    case kKeyboard: return m.keyboard.device;
    case kMouse: return m.mouse.device;
    case kHeadset: return m.headsetGear.device;
    case kController: return m.controllerGear.device;
    default:
        if (obj >= kOther0 && obj < kOther0 + static_cast<int>(m.others.size())) return m.others[static_cast<size_t>(obj - kOther0)].device;
        return nullptr;
    }
}

// What can be moved on the desk: the desk item's saved name ("" for none). Parts of the case
// move the whole case.
std::string DeskItemOf(const Model& m, int obj) {
    if (InCase(obj)) return "desk:case";
    if (IsMonitor(obj)) return "desk:monitor";
    if (obj == kKeyboard) return m.keyboard.spot;
    if (obj == kMouse) return m.mouse.spot;
    if (obj == kHeadset) return m.headsetGear.spot;
    if (obj == kController) return m.controllerGear.spot;
    if (obj >= kOther0 && obj < kOther0 + static_cast<int>(m.others.size())) return m.others[static_cast<size_t>(obj - kOther0)].spot;
    return "";
}

// A tooltip's lines for a part: what it is, and what the sensors say right now.
void Describe(Controller& ctl, const Model& m, const sensors::SystemSnapshot& snap, int obj) {
    char b[160];
    auto rgbLine = [&](const char* device) {
        if (device) Muted("RGB: %s", LiveParams(ctl, device) ? "lit by LumaBridge" : "not lit by LumaBridge right now");
        else Muted("No RGB lighting LumaBridge can control");
    };
    if (obj >= kFan0 && obj < kFan0 + pc::kSlots) {
        const int slot = obj - kFan0;
        const pc::Slot& s = pc::Slots()[static_cast<size_t>(slot)];
        const pc::SlotFan f = m.layout.slots[static_cast<size_t>(slot)];
        if (f == pc::SlotFan::None) {
            ImGui::Text("Empty slot (%s)", pc::MountName(s.mount));
            Muted("Click to put a fan here.");
            return;
        }
        ImGui::Text("%s fan%s", pc::MountName(s.mount), f == pc::SlotFan::Rgb ? " (RGB, on the ARGB header)" : "");
        const double rpm = m.fanRpm[static_cast<size_t>(slot)];
        if (rpm > 0) snprintf(b, sizeof b, "%.0f RPM, %s", rpm, m.layout.Exhaust(slot) ? "blowing out" : "pulling air in");
        else snprintf(b, sizeof b, "Speed not reported; %s", m.layout.Exhaust(slot) ? "blowing out" : "pulling air in");
        Muted("%s", b);
        if (f == pc::SlotFan::Rgb && !m.fansRgb) Muted("Marked as RGB, but LumaBridge found no ARGB header (ASUS Aura) to light it");
        else rgbLine(DeviceOf(m, obj));
        return;
    }
    if (IsMonitor(obj)) {
        const int i = obj - kMonitor0;
        if (i >= static_cast<int>(m.screens.size())) {
            ImGui::TextUnformatted("Monitor");
            Muted("Windows didn't report its displays.");
            return;
        }
        const displays::Display& d = m.screens[static_cast<size_t>(i)];
        ImGui::Text("%s%s", d.name.c_str(), d.primary ? " (main display)" : "");
        float w, h;
        displays::ScreenSize(d, &w, &h);
        snprintf(b, sizeof b, "%d x %d", d.w, d.h);
        std::string line = b;
        if (d.hz > 1) snprintf(b, sizeof b, " at %d Hz", d.hz), line += b;
        snprintf(b, sizeof b, ", %.1f\"", std::sqrt(w * w + h * h) / 2.54f), line += b;
        Muted("%s", line.c_str());
        Muted("Placed as Windows arranges your displays. No RGB lighting LumaBridge can control.");
        Muted("Correct its size under Monitors on My setup if the reported size is wrong.");
        return;
    }
    switch (obj) {
    case kCpu: {
        ImGui::TextUnformatted(m.cpuName.empty() ? "Processor" : m.cpuName.c_str());
        std::string line;
        if (snap.cpuLoad >= 0) snprintf(b, sizeof b, "%.0f %% load", snap.cpuLoad), line += b;
        if (const auto* x = sensors::PickSensor(snap.lhm, sensors::HardwareKind::Cpu, sensors::SensorType::Temperature,
                                                {"Tctl", "Package", "Core (Tdie)", "CPU"}))
            snprintf(b, sizeof b, "   %.0f \xC2\xB0" "C", x->value), line += b;
        if (snap.cpuClockMhz > 0) snprintf(b, sizeof b, "   %.2f GHz", snap.cpuClockMhz / 1000), line += b;
        if (!line.empty()) Muted("%s", line.c_str());
        if (m.layout.cooler == pc::Cooler::Aio) {
            Muted("%s, radiator %s", m.aio ? m.aio->name : "AIO water cooler", m.layout.radiator == pc::Mount::Top ? "on top" : "in front");
            if (m.kraken.valid) {
                Muted("Liquid: %.1f \xC2\xB0" "C   Pump: %d RPM (%d%%)", m.kraken.liquidC, m.kraken.pumpRpm, m.kraken.pumpDuty);
                if (m.kraken.fanRpm > 0) Muted("Radiator fans: %d RPM (%d%%)", m.kraken.fanRpm, m.kraken.fanDuty);
                if (m.aio && m.aio->lcd) Muted("Its screen shows the liquid temperature here (NZXT CAM sets what it really shows).");
            } else {
                if (m.aio && m.aio->lcd) Muted("Its screen shows the CPU temperature here (NZXT CAM sets what it really shows).");
                if (m.pumpRpm > 0) Muted("Pump: %.0f RPM", m.pumpRpm);
            }
        } else if (m.cpuFanRpm > 0) {
            Muted("Cooler fan: %.0f RPM", m.cpuFanRpm);
        }
        if (m.layout.cooler == pc::Cooler::Aio && m.layout.pumpRgb) {
            if (m.fansRgb) Muted("Pump RGB: on the ARGB header, with the fans' lighting");
            else Muted("Pump RGB: LumaBridge found no ARGB header (ASUS Aura) to light it");
        } else {
            rgbLine(nullptr);
        }
        break;
    }
    case kGpu:
        ImGui::TextUnformatted(m.gpuName.c_str());
        for (const auto& g : snap.gpus) {
            if (sensors::FriendlyGpu(g.name) != m.gpuName) continue;
            std::string line;
            if (g.load >= 0) snprintf(b, sizeof b, "%.0f %% load", g.load), line += b;
            if (g.temp >= 0) snprintf(b, sizeof b, "   %.0f \xC2\xB0" "C", g.temp), line += b;
            if (g.powerW >= 0) snprintf(b, sizeof b, "   %.0f W", g.powerW), line += b;
            if (g.clockMhz > 0) snprintf(b, sizeof b, "   %.0f MHz", g.clockMhz), line += b;
            if (!line.empty()) Muted("%s", line.c_str());
            if (g.fanPct >= 0) Muted("Fans: %.0f %%", g.fanPct);
        }
        rgbLine(DeviceOf(m, obj));
        break;
    case kRam: {
        ImGui::TextUnformatted(m.ramName.empty() ? "Memory" : m.ramName.c_str());
        int sticks = 0;
        for (bool s : m.ram) sticks += s;
        if (snap.memTotal) Muted("%d stick%s, %s of %s used", sticks, sticks == 1 ? "" : "s", Gb(snap.memUsed).c_str(), Gb(snap.memTotal).c_str());
        rgbLine(DeviceOf(m, obj));
        break;
    }
    case kBoard:
        ImGui::TextUnformatted(m.boardName.empty() ? "Motherboard" : m.boardName.c_str());
        if (const auto* x = sensors::PickSensor(snap.lhm, sensors::HardwareKind::Board, sensors::SensorType::Temperature, {"Motherboard"}))
            Muted("%.0f \xC2\xB0" "C", x->value);
        rgbLine(DeviceOf(m, obj));
        break;
    case kStorage: {
        ImGui::TextUnformatted("Storage");
        for (const auto& d : Drives()) Muted("%s  %s free of %s", d.root.c_str(), Size(d.free).c_str(), Size(d.total).c_str());
        rgbLine(nullptr);
        break;
    }
    case kPsu:
        ImGui::TextUnformatted("Power supply");
        Muted("Under the shroud. It doesn't report anything.");
        break;
    case kCase:
        ImGui::TextUnformatted("Case");
        Muted("Drag to move it on the desk.");
        break;
    default: {
        const Gear* g = obj == kKeyboard ? &m.keyboard : obj == kMouse ? &m.mouse : obj == kHeadset ? &m.headsetGear
                       : obj == kController ? &m.controllerGear : nullptr;
        if (obj >= kOther0 && obj < kOther0 + static_cast<int>(m.others.size())) g = &m.others[static_cast<size_t>(obj - kOther0)];
        if (!g) return;
        ImGui::TextUnformatted(g->name.c_str());
        if (g->device) rgbLine(g->device);
        else if (obj == kKeyboard) Muted("LumaBridge found no RGB keyboard it can light");
        else if (obj == kMouse) Muted("LumaBridge found no RGB mouse it can light");
        else rgbLine(nullptr);
    }
    }
}

enum class Mode { Lighting, Preview, MySetup };

void DefaultCamera(s3d::Camera* c, Mode mode) {
    *c = {};
    if (mode == Mode::MySetup) {
        c->target = {26, 14, -10};
        c->yaw = -0.5f;
        c->pitch = 0.36f;
        c->distance = 125;
    } else {
        c->target = {40, 18, -14};
        c->yaw = -0.62f;
        c->pitch = 0.34f;
        c->distance = 100;
    }
}

}  // namespace view3d

// Turns something on the desk by `degrees` (counter-clockwise seen from above).
void TurnItem(Controller& ctl, const std::string& item, float degrees) {
    if (item.empty()) return;
    auto& spots = ctl.prefs().setupSpots;
    Spot s;
    if (auto it = spots.find(item); it != spots.end()) s = it->second;
    else pc::ToSaved(pc::DefaultDeskSpot(item), &s.x, &s.y);
    s.angle = static_cast<float>(std::fmod(s.angle + degrees + 720.f, 360.f));
    spots[item] = s;
    ctl.Changed();
}

// Draws rendered 3D items with Dear ImGui, far to near: the fallback when the depth-tested
// renderer isn't available.
void PaintItems(ImDrawList* dl, const std::vector<s3d::DrawItem>& items) {
    for (const auto& it : items) {
        if (it.kind == s3d::DrawItem::Polygon) {
            // Dear ImGui's anti-aliased fill wants the corners clockwise on screen.
            ImVec2 pts[s3d::kMaxCorners];
            float area = 0;
            for (int i = 0; i < it.n; ++i) {
                const int j = (i + 1) % it.n;
                area += it.xy[i * 2] * it.xy[j * 2 + 1] - it.xy[j * 2] * it.xy[i * 2 + 1];
            }
            for (int i = 0; i < it.n; ++i) {
                const int k = area >= 0 ? i : it.n - 1 - i;
                pts[i] = ImVec2(it.xy[k * 2], it.xy[k * 2 + 1]);
            }
            dl->AddConvexPolyFilled(pts, it.n, it.color);
        } else if (it.kind == s3d::DrawItem::GlowDot) {
            const float r = std::max(1.f, it.radius);
            const int alpha = static_cast<int>(it.color >> 24);
            dl->AddCircleFilled(ImVec2(it.xy[0], it.xy[1]), r, s3d::WithAlpha(it.color, alpha / 3), 12);
            dl->AddCircleFilled(ImVec2(it.xy[0], it.xy[1]), r * 0.45f, s3d::WithAlpha(it.color, alpha), 10);
        } else {
            dl->AddLine(ImVec2(it.xy[0], it.xy[1]), ImVec2(it.xy[2], it.xy[3]), it.color, it.width * S());
        }
    }
}

// The whole setup in 3D. Lighting: click a lit part to edit its lighting. MySetup: drag
// things on the desk, click fan slots while editing, see the air move. Preview: just look.
// Drag empty space (or with the right button) to turn the view; the wheel zooms.
void SetupView(Controller& ctl, UiState& ui, view3d::Mode mode, float height) {
    using namespace view3d;
    UiState::View3d& v = mode == Mode::MySetup ? ui.setupView : mode == Mode::Lighting ? ui.lightView : ui.guideView;
    if (!v.camSet) {
        DefaultCamera(&v.cam, mode);
        v.camSet = true;
    }
    Prefs& prefs = ctl.prefs();
    const sensors::SystemSnapshot snap = ctl.monitor().Snapshot();
    Model m = Gather(ctl, snap);
    Options o;
    o.editSlots = mode == Mode::MySetup && ui.setupEdit;
    o.airflow = mode == Mode::MySetup && ui.setupAirflow;
    static Spin spins[3];
    const double t = ImGui::GetTime();
    s3d::Scene sc;
    std::vector<Anchor> anchors;
    std::vector<SurfaceText> texts;
    Build(sc, ctl, m, o, t, spins[static_cast<int>(mode)], &anchors, &texts);

    const float W = ImGui::GetContentRegionAvail().x, Hh = height;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const s3d::Viewport vp{a.x, a.y, W, Hh};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + W, a.y + Hh), Hex(0x141A26), Hex(0x141A26), Hex(0x07090D), Hex(0x07090D));

    ImGui::InvisibleButton("view3d", ImVec2(W, Hh),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    if (hovered) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    bool geometryChanged = false;
    if (hovered && io.MouseWheel != 0) {
        if (v.dragObj >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            TurnItem(ctl, DeskItemOf(m, v.dragObj), io.MouseWheel * 15.f);  // turn what you're holding
            geometryChanged = true;
        } else {
            v.cam.distance *= std::pow(0.9f, io.MouseWheel);
            v.cam.Clamp();
        }
    }

    const s3d::Camera projectedCamera = v.cam;
    auto items = s3d::Render(sc, v.cam, vp);
    int under = hovered ? s3d::Pick(items, io.MousePos.x, io.MousePos.y) : -1;
    const bool canDrag = mode == Mode::MySetup && !ui.setupEdit;

    // Sliding the view across the desk (panning), by `dx`, `dy` pixels: the desk follows the mouse.
    auto pan = [&](float dx, float dy) {
        s3d::V3 r, u, f;
        v.cam.Basis(&r, &u, &f);
        s3d::V3 ahead{f.x, 0, f.z};
        if (s3d::Length(ahead) < 1e-3f) ahead = {-u.x, 0, -u.z};
        ahead = s3d::Normalize(ahead);
        const float k = v.cam.distance / vp.Focal(v.cam);
        v.cam.target = v.cam.target - s3d::V3{r.x, 0, r.z} * (dx * k) + ahead * (dy * k / std::max(0.35f, std::sin(v.cam.pitch)));
        v.cam.target.x = std::clamp(v.cam.target.x, -pc::kDeskW / 2 - 30, pc::kDeskW / 2 + 30);
        v.cam.target.z = std::clamp(v.cam.target.z, -pc::kDeskD / 2 - 30, pc::kDeskD / 2 + 30);
    };

    // Press: on something that moves (My setup), move it; on the desk, turn the view (Shift:
    // slide it). The right or middle button slides the view too.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        v.dragObj = -1;
        v.orbiting = v.panning = false;
        const std::string item = canDrag && !io.KeyShift ? DeskItemOf(m, under) : "";
        // Grabbed about where the mouse is on it, so it follows the mouse one to one.
        float grabY = 0;
        for (const Anchor& an : anchors)
            if (an.obj == under) grabY = std::clamp(an.at.y * 0.5f, 0.f, 30.f);
        s3d::V3 hit;
        if (!item.empty() && s3d::HitPlaneY(s3d::ScreenRay(v.cam, vp, io.MousePos.x, io.MousePos.y), grabY, &hit)) {
            v.dragObj = under;
            v.dragHeight = grabY;
            v.dragOffset = DeskSpot(prefs, item) - hit;
        } else if (io.KeyShift) {
            v.panning = true;
        } else {
            v.orbiting = true;
        }
    }
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle))) {
        v.panning = true;
        v.orbiting = false;
    }
    if (hovered && !ImGui::GetIO().WantTextInput) {  // the arrow keys slide the view
        const float step = 400 * io.DeltaTime;
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) pan(step, 0);
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) pan(-step, 0);
        if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) pan(0, step);
        if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) pan(0, -step);
    }
    if (ImGui::IsItemActive()) {
        const bool left = ImGui::IsMouseDown(ImGuiMouseButton_Left), right = ImGui::IsMouseDown(ImGuiMouseButton_Right),
                   middle = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        const ImGuiMouseButton held = left ? ImGuiMouseButton_Left : right ? ImGuiMouseButton_Right : ImGuiMouseButton_Middle;
        const bool moved = (left || right || middle) && ImGui::IsMouseDragging(held, 3 * S());
        if (moved && left && v.dragObj >= 0) {
            s3d::V3 hit;
            if (s3d::HitPlaneY(s3d::ScreenRay(v.cam, vp, io.MousePos.x, io.MousePos.y), v.dragHeight, &hit)) {
                const std::string item = DeskItemOf(m, v.dragObj);
                Spot s;
                if (auto it = prefs.setupSpots.find(item); it != prefs.setupSpots.end()) s = it->second;  // keeps its angle
                pc::ToSaved(hit + v.dragOffset, &s.x, &s.y);
                prefs.setupSpots[item] = s;
                geometryChanged = true;
                v.selected = v.dragObj;
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else if (moved && v.panning) {
            pan(io.MouseDelta.x, io.MouseDelta.y);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else if (moved && v.orbiting) {
            v.cam.yaw -= io.MouseDelta.x * 0.008f;
            v.cam.pitch += io.MouseDelta.y * 0.006f;
            v.cam.Clamp();
        }
    }
    if (ImGui::IsItemDeactivated()) {
        const bool click = !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left, 3 * S()) &&
                           !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right, 3 * S()) &&
                           !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Middle, 3 * S()) && !v.panning;
        if (v.dragObj >= 0 && !click) ctl.Changed();  // save where it went
        if (click && under >= 0) {
            if (mode == Mode::MySetup && ui.setupEdit && under >= kFan0 && under < kFan0 + pc::kSlots) {
                pc::Layout l = m.layout;
                pc::CycleSlot(&l, under - kFan0);
                prefs.caseLayout = pc::Encode(l);
                geometryChanged = true;
                ctl.Changed();
            } else if (mode == Mode::Lighting) {
                if (const char* d = DeviceOf(m, under)) ui.lightTarget = ui.lightTarget == d ? "" : d;
            } else if (canDrag) {
                v.selected = DeskItemOf(m, under).empty() ? -1 : under;  // to turn it
            }
        } else if (click && canDrag) {
            v.selected = -1;
        }
        v.dragObj = -1;
        v.orbiting = v.panning = false;
    }
    // Double-click: on something, bring it to the middle of the view; on the desk, start over.
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const Anchor* on = nullptr;
        for (const Anchor& an : anchors)
            if (an.obj == under) on = &an;
        if (under < 0 || under == kDesk) {
            DefaultCamera(&v.cam, mode);
        } else if (on) {
            v.cam.target = {on->at.x, std::clamp(on->at.y * 0.6f, 5.f, 40.f), on->at.z};
            v.cam.distance = std::min(v.cam.distance, 110.f);
        }
    }
    // Project the final camera and positions in this frame, so geometry, labels and picking
    // agree during orbiting and dragging instead of lagging behind the input.
    if (geometryChanged) {
        m = Gather(ctl, snap);
        sc = {};
        anchors.clear();
        texts.clear();
        Build(sc, ctl, m, o, t, spins[static_cast<int>(mode)], &anchors, &texts);
    }
    if (geometryChanged || v.cam.yaw != projectedCamera.yaw || v.cam.pitch != projectedCamera.pitch ||
        v.cam.distance != projectedCamera.distance || s3d::Length(v.cam.target - projectedCamera.target) > 0) {
        items = s3d::Render(sc, v.cam, vp);
        under = hovered ? s3d::Pick(items, io.MousePos.x, io.MousePos.y) : -1;
    }
    if (hovered && under >= 0 && v.dragObj < 0 && !v.orbiting) {
        const bool pickable = mode == Mode::Lighting ? DeviceOf(m, under) != nullptr
                              : mode == Mode::MySetup ? (ui.setupEdit ? under >= kFan0 && under < kFan0 + pc::kSlots : !DeskItemOf(m, under).empty())
                                                      : false;
        if (pickable) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // Draw the depth-tested scene, with a painter fallback if D3D11 setup failed.
    dl->PushClipRect(a, ImVec2(a.x + W, a.y + Hh), true);
    const uintptr_t sceneImage = scene_gpu::Image(static_cast<int>(mode), items, vp, S());
    if (sceneImage) dl->AddImage(static_cast<ImTextureID>(sceneImage), a, ImVec2(a.x + W, a.y + Hh));
    else PaintItems(dl, items);
    // Text on surfaces (the pump's screen), where the surface faces the camera.
    for (const SurfaceText& st : texts) {
        const s3d::V3 eye = v.cam.Eye();
        if (s3d::Dot(st.normal, eye - st.at) <= 0) continue;
        const s3d::Projected c = s3d::Project(v.cam, vp, st.at), top = s3d::Project(v.cam, vp, st.at + st.up * st.height);
        if (!c.visible || !top.visible) continue;
        // The pump label follows the same visibility rule as the model underneath it.
        float visibleDepth = 1e30f;
        for (const auto& item : items) {
            if (item.kind != s3d::DrawItem::Polygon || (item.color >> 24) < 250) continue;
            const float depth = s3d::DepthAt(item, c.sx, c.sy);
            if (depth > 0) visibleDepth = std::min(visibleDepth, depth);
        }
        if (visibleDepth + 0.15f < c.z) continue;
        const float px = std::hypot(top.sx - c.sx, top.sy - c.sy);
        if (px < 5) continue;
        ImFont* font = ImGui::GetFont();
        const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0, st.text.c_str());
        dl->AddText(font, px, ImVec2(c.sx - ts.x / 2, c.sy - ts.y / 2), st.color, st.text.c_str());
    }
    // Outlines: what the mouse is over, and (Lighting) the parts of the device being edited:
    // around each one's outer edge (not every face, which looked like an X-ray).
    auto outlineOf = [&](const std::vector<int>& group, ImU32 col, float w) {
        std::vector<ImVec2> pts;
        for (const auto& it : items)
            if (it.kind == s3d::DrawItem::Polygon && std::find(group.begin(), group.end(), it.id) != group.end())
                for (int i = 0; i < it.n; ++i) pts.push_back(ImVec2(it.xy[i * 2], it.xy[i * 2 + 1]));
        if (pts.size() < 3) return;
        // Convex hull (Andrew's monotone chain).
        std::sort(pts.begin(), pts.end(), [](ImVec2 a, ImVec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
        auto cross = [](ImVec2 o2, ImVec2 a, ImVec2 b) { return (a.x - o2.x) * (b.y - o2.y) - (a.y - o2.y) * (b.x - o2.x); };
        std::vector<ImVec2> hull(pts.size() * 2);
        size_t k = 0;
        for (size_t i = 0; i < pts.size(); ++i) {
            while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
            hull[k++] = pts[i];
        }
        for (size_t i = pts.size() - 1, lo = k + 1; i-- > 0;) {
            while (k >= lo && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
            hull[k++] = pts[i];
        }
        hull.resize(k > 1 ? k - 1 : k);
        if (hull.size() >= 3) dl->AddPolyline(hull.data(), static_cast<int>(hull.size()), col, ImDrawFlags_Closed, w * S());
    };
    auto outline = [&](int id, ImU32 col, float w) { outlineOf({id}, col, w); };
    std::vector<int> ids{kCase, kBoard, kCpu, kRam, kGpu, kPsu, kStorage, kKeyboard, kMouse, kHeadset, kController};
    for (int i = 0; i < static_cast<int>(std::max<size_t>(1, m.screens.size())); ++i) ids.push_back(kMonitor0 + i);
    for (int i = 0; i < pc::kSlots; ++i) ids.push_back(kFan0 + i);
    for (int i = 0; i < static_cast<int>(m.others.size()); ++i) ids.push_back(kOther0 + i);
    if (mode == Mode::Lighting && !ui.lightTarget.empty())
        for (int id : ids)
            if (const char* d = DeviceOf(m, id); d && ui.lightTarget == d) outline(id, Hex(kAccentHover, 200), 1.6f);
    if (mode == Mode::MySetup && v.selected >= 0) {
        // What's selected: all of it (the whole case for a part of it), around its outer edge;
        // each monitor on its own.
        std::vector<int> group;
        for (int id : ids)
            if (DeskItemOf(m, id) == DeskItemOf(m, v.selected)) {
                if (IsMonitor(id)) outline(id, Hex(kAccentHover, 190), 1.6f);
                else group.push_back(id);
            }
        if (!group.empty()) outlineOf(group, Hex(kAccentHover, 190), 1.6f);
    }
    const int highlight = v.dragObj >= 0 ? v.dragObj : under;
    if (highlight >= 0 && highlight != kDesk && !(mode == Mode::MySetup && v.selected >= 0 && DeskItemOf(m, highlight) == DeskItemOf(m, v.selected) &&
                                                  highlight == kCase))
        outline(highlight, Hex(kText, 120), 1.2f);

    // Names over the parts (My setup), or the one under the mouse.
    // The one under the mouse first, then the rest where they don't cover each other.
    std::vector<ImVec4> placed;
    std::vector<const Anchor*> order;
    for (const Anchor& an : anchors)
        if (an.obj == under) order.insert(order.begin(), &an);
        else order.push_back(&an);
    for (const Anchor* ap : order) {
        const Anchor& an = *ap;
        const bool show = (mode == Mode::MySetup && ui.setupLabels) || an.obj == under;
        if (!show || an.label.empty()) continue;
        const s3d::Projected pr = s3d::Project(v.cam, vp, an.at);
        if (!pr.visible) continue;
        const ImVec2 ts = ImGui::CalcTextSize(an.label.c_str());
        const ImVec2 p0(pr.sx - ts.x / 2 - 6 * S(), pr.sy - ts.y - 8 * S());
        const ImVec4 r(p0.x, p0.y, p0.x + ts.x + 12 * S(), p0.y + ts.y + 6 * S());
        bool covers = false;
        for (const ImVec4& q : placed) covers |= r.x < q.z && q.x < r.z && r.y < q.w && q.y < r.w;
        if (covers) continue;
        placed.push_back(r);
        dl->AddRectFilled(p0, ImVec2(p0.x + ts.x + 12 * S(), p0.y + ts.y + 6 * S()), Hex(0x0B0D12, an.obj == under ? 230 : 170), 6 * S());
        const bool lit = DeviceOf(m, an.obj) != nullptr;
        dl->AddText(ImVec2(p0.x + 6 * S(), p0.y + 3 * S()), Hex(lit ? kText : kMuted), an.label.c_str());
    }
    dl->PopClipRect();

    if (hovered && under >= 0 && v.dragObj < 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 320 * S());
        Describe(ctl, m, snap, under);
        if (mode == Mode::Lighting && DeviceOf(m, under)) Muted("Click to edit its lighting.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    // How to use it, bottom left.
    const char* hint = mode == Mode::MySetup
                           ? (ui.setupEdit ? "Click a slot to add a fan, make it an RGB fan, or take it out.  Drag to turn the view."
                                           : "Drag things to move them (scroll while dragging turns them); double-click one to center it.\n"
                                             "Drag the desk to turn the view; right-drag, Shift-drag or the arrow keys slide it; scroll zooms.")
                       : mode == Mode::Lighting ? "Click a lit part to edit its lighting.  Drag to turn the view, scroll to zoom."
                                                : "Drag to turn the view, scroll to zoom.";
    dl->AddText(ImVec2(a.x + 12 * S(), a.y + Hh - ImGui::CalcTextSize(hint).y - 10 * S()), Hex(kMuted, 200), hint);
    for (size_t i = 0; i < m.notes.size() && mode == Mode::MySetup; ++i)
        dl->AddText(ImVec2(a.x + 12 * S(), a.y + 10 * S() + static_cast<float>(i) * ImGui::GetTextLineHeightWithSpacing()), Hex(kAmber, 220),
                    m.notes[i].c_str());
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + Hh));
    ImGui::Dummy(ImVec2(W, 6 * S()));
    // Turning what's selected.
    if (mode == Mode::MySetup && v.selected >= 0) {
        const std::string item = DeskItemOf(m, v.selected);
        std::string name = InCase(v.selected) ? "Case" : IsMonitor(v.selected) ? (m.screens.size() > 1 ? "Monitors" : "Monitor") : "";
        for (const Anchor& an : anchors)
            if (name.empty() && an.obj == v.selected) name = an.label;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", name.c_str());
        ImGui::SameLine(0, 12 * S());
        if (Btn("Turn left")) TurnItem(ctl, item, 15);
        ImGui::SameLine();
        if (Btn("Turn right")) TurnItem(ctl, item, -15);
        ImGui::SameLine();
        if (Btn("Quarter turn")) TurnItem(ctl, item, 90);
        ImGui::SameLine();
        if (Btn("Straight")) TurnItem(ctl, item, -DeskAngle(prefs, item) / 0.01745329f);
        ImGui::SameLine();
        Muted("Scroll while dragging turns it too.");
    }
}

// ---- Your setup in 2D: each lit device drawn flat, placed where you drag it ------------------

// One thing on the canvas: a fan, the motherboard, the memory, the mouse or the keyboard.
struct SetupItem {
    std::string item;         // key in Prefs::setupSpots
    const char* device;       // device::kFans, ...; nullptr: no RGB LumaBridge controls
    ImVec2 size;
    int fan = -1;             // fans: which one
    std::string name;         // shown under it, else the device's name
    enum class Look { Normal, Bar, Gpu, Headset, Controller, Cooler } look = Look::Normal;
    int leds = 8;             // light bars
};



void Glow(ImDrawList* dl, ImVec2 c, float r, Rgb col) {
    dl->AddCircleFilled(c, r * 2.4f, Col(col, 38), 20);
    dl->AddCircleFilled(c, r, Col(col), 16);
}

// Curved blades turn inside a separate diffuser ring, with mounting screws in the frame.
void DrawFan(ImDrawList* dl, ImVec2 a, ImVec2 size, const std::vector<Rgb>& leds, int fan, int per) {
    const ImVec2 c(a.x + size.x / 2, a.y + size.y / 2);
    const float r = std::min(size.x, size.y) / 2;
    dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), Hex(0x191E28), r * 0.16f);
    dl->AddRect(a, ImVec2(a.x + size.x, a.y + size.y), Hex(0x4A5363), r * 0.16f, 0, S());
    for (float x : {0.10f, 0.90f}) for (float y : {0.10f, 0.90f}) {
        const ImVec2 screw(a.x + size.x * x, a.y + size.y * y);
        dl->AddCircleFilled(screw, r * 0.055f, Hex(0x070A10), 12);
        dl->AddLine(ImVec2(screw.x - r * 0.026f, screw.y), ImVec2(screw.x + r * 0.026f, screw.y), Hex(0x778194), S() * 0.7f);
    }
    dl->AddCircleFilled(c, r * 0.92f, Hex(0x090C12), 48);
    const float rotation = static_cast<float>(ImGui::GetTime()) * 0.8f;
    for (int blade = 0; blade < 7; ++blade) {
        const float angle = 6.2831853f * blade / 7 + rotation;
        auto polar = [&](float radius, float offset) { return ImVec2(c.x + std::cos(angle + offset) * r * radius,
                                                                   c.y + std::sin(angle + offset) * r * radius); };
        dl->PathLineTo(polar(0.20f, 0));
        dl->PathBezierCubicCurveTo(polar(0.43f, -0.18f), polar(0.72f, -0.12f), polar(0.73f, 0.20f));
        dl->PathBezierCubicCurveTo(polar(0.72f, 0.46f), polar(0.40f, 0.58f), polar(0.20f, 0.54f));
        dl->PathFillConcave(Hex(blade % 2 ? 0x35404F : 0x2A3443));
    }
    const float ringR = r * 0.82f;
    dl->AddCircle(c, ringR, Hex(0x394354), 64, r * 0.12f);
    if (per > 0 && fan >= 0) for (int i = 0; i < per; ++i) {
        const size_t index = static_cast<size_t>(fan * per + i);
        const Rgb led = index < leds.size() ? leds[index] : Rgb{50, 54, 64};
        const float angle = -1.5707963f + 6.2831853f * i / per;
        const float half = 3.14159f / per * 0.91f;
        dl->PathArcTo(c, ringR, angle - half, angle + half, 6);
        dl->PathStroke(Col(led, 40), ImDrawFlags_None, r * 0.22f);
        dl->PathArcTo(c, ringR, angle - half, angle + half, 6);
        dl->PathStroke(Col(led), ImDrawFlags_None, r * 0.105f);
    }
    dl->AddCircleFilled(c, r * 0.24f, Hex(0x181E28), 32);
    dl->AddCircle(c, r * 0.24f, Hex(0x647084), 32, S());
    dl->AddCircleFilled(c, r * 0.14f, Hex(0x2C3543), 24);
    dl->AddLine(ImVec2(c.x - r * 0.07f, c.y), ImVec2(c.x + r * 0.07f, c.y), Hex(0x7C879A), S());
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
    dl->AddRect(ImVec2(sa.x - w * 0.12f, sa.y - 2 * S()), ImVec2(sb.x + w * 0.12f, sb.y + 2 * S()),
                Hex(0x566173), 2 * S(), 0, 0.7f * S());
    for (float y : {sa.y - 2 * S(), sb.y})
        dl->AddRectFilled(ImVec2(sa.x - w * 0.18f, y), ImVec2(sb.x + w * 0.18f, y + 2 * S()), Hex(0x626D7E), S());
    auto ledAt = [&](float frac) {
        const int led = std::clamp(static_cast<int>(frac * 5), 0, 4);
        return LiveAt(p, t, led, 5, level);
    };
    auto part = [&](float y0, float y1, float slant = 0) {
        // A long diffuser section can cross several LEDs. Keep all five colors visible.
        for (int led = 0; led < 5; ++led) {
            const float begin = std::max(y0, led * 0.2f);
            const float end = std::min(y1, (led + 1) * 0.2f);
            if (end <= begin) continue;
            const Rgb c = ledAt((begin + end) / 2);
            const ImVec2 p0(sa.x, sa.y + begin * h + slant * w), p1(sb.x, sa.y + begin * h),
                         p2(sb.x, sa.y + end * h), p3(sa.x, sa.y + end * h + slant * w);
            dl->AddRectFilled(ImVec2(sa.x - 3 * S(), sa.y + begin * h - S()),
                              ImVec2(sb.x + 3 * S(), sa.y + end * h + S()), Col(c, 28), 3 * S());
            dl->AddQuadFilled(p0, p1, p2, p3, Col(c));
        }
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

// An angular illuminated emblem on the ROG shroud, split across the board's LED colors.
void DrawBoardEmblem(ImDrawList* dl, ImVec2 c, float r, const std::vector<Rgb>& leds) {
    const ImVec2 outline[] = {ImVec2(c.x-r, c.y+r*0.35f), ImVec2(c.x-r*0.15f,c.y-r*0.40f),
                             ImVec2(c.x+r,c.y-r*0.55f), ImVec2(c.x+r*0.25f,c.y+r*0.38f)};
    const Rgb first = leds.empty() ? Rgb{50,54,64} : leds.front();
    dl->AddPolyline(outline, 4, Col(first, 30), ImDrawFlags_Closed, r * 0.30f);
    for (int i = 0; i < 4; ++i) {
        const Rgb color = leds.empty() ? first : leds[static_cast<size_t>(i) * leds.size() / 4];
        dl->AddLine(outline[i], outline[(i+1)%4], Col(color), std::max(S(),r*0.10f));
    }
    dl->AddLine(ImVec2(c.x-r*0.3f,c.y+r*0.30f), ImVec2(c.x+r*0.68f,c.y-r*0.42f), Col(first), r*0.12f);
}

// The motherboard as the scan found it: the I/O cover top left (with its lit logo, drawn as
// an angular emblem, or the TUF badge on those boards, else its LEDs along the edge), the CPU
// socket, and the memory standing in its slots right of the CPU.
void DrawBoard(ImDrawList* dl, ImVec2 a, ImVec2 size, const std::vector<Rgb>& leds, const SetupHardware& hw,
               const std::array<bool, 4>& slots, bool ramLit, const fx::Params* ram, double t, double ramLevel) {
    const ImVec2 b(a.x + size.x, a.y + size.y);
    auto at = [&](float fx, float fy) { return ImVec2(a.x + fx * size.x, a.y + fy * size.y); };
    dl->AddRectFilled(a, b, Hex(0x0F1218), 6 * S());
    dl->AddRect(a, b, Hex(0x2A3142), 6 * S(), 0, 1.2f * S());
    // Mounting holes and thin PCB traces remain visible around the heatsinks.
    for (float x : {0.025f, 0.975f}) for (float y : {0.035f, 0.965f}) {
        dl->AddCircleFilled(at(x,y), 2.2f*S(), Hex(0x7D6D47), 12);
        dl->AddCircleFilled(at(x,y), 1.1f*S(), Hex(0x080B10), 12);
    }
    for (int i=0;i<5;++i) {
        const float y=0.53f+i*0.043f;
        dl->AddLine(at(0.40f,y),at(0.48f,y),Hex(0x263A3C),0.7f*S());
        dl->AddLine(at(0.48f,y),at(0.53f,y+0.05f),Hex(0x263A3C),0.7f*S());
        dl->AddLine(at(0.53f,y+0.05f),at(0.64f,y+0.05f),Hex(0x263A3C),0.7f*S());
    }
    // The I/O cover.
    const ImVec2 shroud[] = {at(0.02f, 0.02f), at(0.36f, 0.02f), at(0.36f, 0.56f), at(0.26f, 0.72f), at(0.02f, 0.72f)};
    dl->AddConvexPolyFilled(shroud, 5, Hex(0x181C25));
    dl->AddPolyline(shroud, 5, Hex(0x2E3546), ImDrawFlags_Closed, 1 * S());
    for (int i = 0; i < 3; ++i)  // its angled lines, below the logo
        dl->AddLine(at(0.04f, 0.50f - i * 0.06f), at(0.20f - i * 0.05f, 0.70f), Hex(0x232937), 1.5f * S());
    // VRM heatsink, socket retention frame and a metal CPU lid.
    dl->AddRectFilled(at(0.39f,0.04f),at(0.63f,0.13f),Hex(0x343D4C),2*S());
    for (int i=0;i<9;++i) dl->AddLine(at(0.41f+i*0.024f,0.055f),at(0.41f+i*0.024f,0.115f),Hex(0x677186),0.8f*S());
    dl->AddRectFilled(at(0.39f,0.18f),at(0.63f,0.47f),Hex(0x080C12),2*S());
    dl->AddRect(at(0.40f,0.19f),at(0.62f,0.46f),Hex(0x778191),2*S(),0,S());
    dl->AddRectFilled(at(0.43f,0.22f),at(0.59f,0.43f),Hex(0x737F90),S());
    dl->AddRectFilled(at(0.445f,0.235f),at(0.575f,0.415f),Hex(0xA2ACB9),S());
    dl->AddLine(at(0.625f,0.21f),at(0.625f,0.44f),Hex(0xB1BAC7),S());
    // Two expansion slots, M.2 cover and a finned chipset heatsink.
    for (float y : {0.79f,0.91f}) {
        dl->AddRectFilled(at(0.08f,y),at(0.61f,y+0.033f),Hex(0x505B6D),S());
        dl->AddLine(at(0.10f,y+0.015f),at(0.56f,y+0.015f),Hex(0x10151D),S());
        dl->AddRectFilled(at(0.58f,y-0.005f),at(0.62f,y+0.04f),Hex(0x788397),S());
    }
    dl->AddRectFilled(at(0.39f,0.62f),at(0.63f,0.70f),Hex(0x303949),S());
    dl->AddLine(at(0.405f,0.66f),at(0.61f,0.66f),Hex(0x637087),S());
    dl->AddRectFilled(at(0.73f,0.82f),at(0.91f,0.95f),Hex(0x333C4D),2*S());
    for (int i=0;i<5;++i) dl->AddLine(at(0.75f,0.83f+i*0.024f),at(0.88f,0.83f+i*0.024f),Hex(0x5D6980),S());
    dl->AddRectFilled(at(0.945f,0.11f),at(0.98f,0.57f),Hex(0x495369),S());

    switch (hw.board) {
    case BoardStyle::Rog:
    case BoardStyle::RogStrix:
        DrawBoardEmblem(dl, at(0.19f, 0.34f), size.y * 0.15f, leds);
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
        dl->AddRectFilled(ImVec2(sa.x-2*S(),sa.y-3*S()),ImVec2(sb.x+2*S(),sb.y+3*S()),Hex(0x171D27),S());
        for (float y : {sa.y-3*S(),sb.y})
            dl->AddRectFilled(ImVec2(sa.x-2*S(),y),ImVec2(sb.x+2*S(),y+3*S()),Hex(0x5D687B),S());
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
    const float w=size.x,h=size.y;
    auto p=[&](float x,float y){return ImVec2(a.x+x*w,a.y+y*h);};
    auto shell=[&]{
        dl->PathLineTo(p(0.48f,0.03f));
        dl->PathBezierCubicCurveTo(p(0.78f,0.01f),p(0.89f,0.17f),p(0.89f,0.39f));
        dl->PathBezierCubicCurveTo(p(0.89f,0.59f),p(0.99f,0.76f),p(0.84f,0.89f));
        dl->PathBezierCubicCurveTo(p(0.64f,1.03f),p(0.32f,0.98f),p(0.20f,0.89f));
        dl->PathLineTo(p(0.07f,0.68f)); dl->PathLineTo(p(0.12f,0.48f));
        dl->PathBezierCubicCurveTo(p(0.17f,0.41f),p(0.15f,0.28f),p(0.19f,0.16f));
        dl->PathBezierCubicCurveTo(p(0.23f,0.07f),p(0.34f,0.03f),p(0.48f,0.03f));
    };
    shell();dl->PathFillConcave(Hex(0x252D39));
    shell();dl->PathStroke(Hex(0x596579),ImDrawFlags_Closed,S());
    // Separate main buttons, thumb rest, side buttons and a ribbed scroll wheel.
    dl->AddRectFilled(p(0.28f,0.08f),p(0.46f,0.38f),Hex(0x343E4E),3*S());
    dl->AddRectFilled(p(0.57f,0.08f),p(0.77f,0.38f),Hex(0x303A4A),3*S());
    const ImVec2 rest[]={p(0.14f,0.48f),p(0.27f,0.50f),p(0.27f,0.78f),p(0.10f,0.67f)};
    dl->AddConvexPolyFilled(rest,4,Hex(0x111722));
    for(float y:{0.37f,0.46f})dl->AddRectFilled(p(0.15f,y),p(0.26f,y+0.055f),Hex(0x566176),S());
    dl->AddRectFilled(p(0.21f,0.59f),p(0.31f,0.66f),Hex(0x6B768B),S());
    dl->AddRectFilled(p(0.49f,0.14f),p(0.55f,0.29f),Hex(0x0B1019),2*S());
    for(int i=0;i<5;++i)dl->AddLine(p(0.49f,0.16f+i*0.025f),p(0.55f,0.16f+i*0.025f),Hex(0x92A0B6),0.8f*S());
    dl->AddRectFilled(p(0.48f,0.33f),p(0.56f,0.37f),Hex(0x707C91),S());
    dl->AddLine(p(0.30f,0.44f),p(0.78f,0.44f),Hex(0x485469),S());
    dl->AddLine(p(0.36f,0.49f),p(0.40f,0.66f),Hex(0x394457),S());
    // A diffuser follows the lower curve and right side, using all eight live LED colors.
    const ImVec2 strip[]={p(0.27f,0.74f),p(0.34f,0.81f),p(0.45f,0.85f),p(0.57f,0.86f),
                          p(0.69f,0.84f),p(0.79f,0.79f),p(0.84f,0.69f),p(0.83f,0.58f),p(0.81f,0.48f)};
    for(int i=0;i<8;++i){
        const Rgb c=static_cast<size_t>(i)<leds.size()?leds[static_cast<size_t>(i)]:Rgb{50,54,64};
        dl->AddLine(strip[i],strip[i+1],Col(c,35),5*S());
        dl->AddLine(strip[i],strip[i+1],Col(c),2*S());
    }
    dl->AddCircle(p(0.55f,0.64f),w*0.08f,Hex(0x7F8BA0),20,S());
    dl->AddLine(p(0.55f,0.56f),p(0.55f,0.64f),Hex(0x252D39),2*S());
}

// The ROG Azoth as it's lit: every key from its table (azoth_layout.h: ISO / Nordic, the
// tall Enter), `colors` in that table's order, the control knob and OLED screen top right.
void DrawKeyboard(ImDrawList* dl, ImVec2 a, ImVec2 size, const std::vector<Rgb>& colors) {
    const ImVec2 b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, Hex(0x2A2F38), 9 * S());  // the case
    dl->AddRectFilled(ImVec2(a.x + 2 * S(), a.y + 2 * S()), ImVec2(b.x - 2 * S(), b.y - 2 * S()), Hex(0x171B22), 8 * S());
    for (float x : {0.025f,0.975f}) for (float y : {0.035f,0.965f}) {
        const ImVec2 screw(a.x+size.x*x,a.y+size.y*y);
        dl->AddCircleFilled(screw,1.3f*S(),Hex(0x6B7586),10);
    }
    dl->AddLine(ImVec2(a.x+10*S(),b.y-2*S()),ImVec2(b.x-10*S(),b.y-2*S()),Hex(0x697384),S());
    const float pad = 7 * S();
    const float u = (size.x - 2 * pad) / 16.f;
    const float rowGap = u * 0.25f;  // the F-row stands apart
    const float gap = std::max(1.5f, u * 0.09f);
    // Fine legends use the existing Nordic / ISO layout, including the tall Enter key.
    static const char* legends[][16] = {
        {"Esc","F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12"},
        {"§","1","2","3","4","5","6","7","8","9","0","+","\\","Bksp","Del"},
        {"Tab","Q","W","E","R","T","Y","U","I","O","P","Å","¨","PgUp"},
        {"Caps","A","S","D","F","G","H","J","K","L","Ø","Æ","'","PgDn"},
        {"Shift","<","Z","X","C","V","B","N","M",",",".","-","Shift","^","End"},
        {"Ctrl","Win","Alt","Space","AltGr","Fn","Ctrl","<","v",">"}
    };
    int columns[6]{};
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
        const int row = std::clamp(static_cast<int>(k.y),0,5);
        const int column = columns[row]++;
        const char* label = k.h > 1 ? "Enter" : column < 16 ? legends[row][column] : nullptr;
        if (label) {
            const float fontSize = std::clamp(u*0.32f,5.5f*S(),8.0f*S());
            const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(fontSize,1000,0,label);
            const float legendHeight = k.h > 1 ? u-gap : kb.y-ka.y;
            dl->AddText(ImGui::GetFont(),fontSize,ImVec2(ka.x+(kb.x-ka.x-textSize.x)/2,
                        ka.y+(legendHeight-textSize.y)/2),Hex(0xD1D8E3,210),label);
        }
        if (k.h > 1) {
            // A dark notch makes the ISO Enter's lower portion narrower than the upper.
            dl->AddRectFilled(ImVec2(ka.x,y+u),ImVec2(ka.x+u*0.22f,kb.y),Hex(0x171B22));
        }
    }
    // Top right: the control knob, then the OLED screen.
    const float y0 = a.y + pad;
    const ImVec2 knob(a.x + pad + 14.2f * u, y0 + u / 2);
    dl->AddCircleFilled(knob, u * 0.34f, Hex(0x3A414F), 20);
    dl->AddCircle(knob, u * 0.34f, Hex(0x505868), 20, 1 * S());
    const ImVec2 oa(a.x + pad + 14.65f * u, y0 + u * 0.08f), ob(a.x + pad + 16 * u - gap / 2, y0 + u * 0.92f);
    dl->AddRectFilled(oa, ob, Hex(0x05070A), 2 * S());
    dl->AddRect(oa,ob,Hex(0x4B566A),2*S(),0,0.8f*S());
    for(int i=0;i<3;++i)
        dl->AddLine(ImVec2(oa.x+u*0.14f,oa.y+u*(0.20f+i*0.14f)),
                    ImVec2(ob.x-u*(i==2?0.40f:0.14f),oa.y+u*(0.20f+i*0.14f)),Hex(0x9FAFC4),0.7f*S());
    for(int i=0;i<8;++i) {
        const float angle=6.2831853f*i/8;
        dl->AddLine(ImVec2(knob.x+std::cos(angle)*u*0.27f,knob.y+std::sin(angle)*u*0.27f),
                    ImVec2(knob.x+std::cos(angle)*u*0.33f,knob.y+std::sin(angle)*u*0.33f),Hex(0x8C98AB),0.7f*S());
    }
}

std::vector<Rgb> Leds2d(Controller& ctl, const char* device, double t, int n, bool one = false) {
    return view3d::Leds(ctl, device, t, n, one);
}

// Unknown strips and accessories retain a neutral shape with a recessed diffuser.
void DrawBar(ImDrawList* dl, ImVec2 a, ImVec2 size, const std::vector<Rgb>& leds) {
    const ImVec2 b(a.x+size.x,a.y+size.y);
    dl->AddRectFilled(a,b,Hex(0x293342),4*S());
    dl->AddRect(a,b,Hex(0x69768B),4*S(),0,0.8f*S());
    const float end=std::min(size.y*0.45f,8*S());
    dl->AddRectFilled(ImVec2(a.x+end,a.y+size.y*0.24f),ImVec2(b.x-end,b.y-size.y*0.24f),Hex(0x0B1018),3*S());
    const int count=static_cast<int>(leds.size());
    for(int i=0;i<count;++i) {
        const float x0=a.x+end+(size.x-2*end)*i/count;
        const float x1=a.x+end+(size.x-2*end)*(i+1)/count;
        const float gap=std::min(0.5f*S(),(x1-x0)*0.15f);
        dl->AddRectFilled(ImVec2(x0,a.y+size.y*0.20f),ImVec2(x1,b.y-size.y*0.20f),Col(leds[static_cast<size_t>(i)],35),2*S());
        dl->AddRectFilled(ImVec2(x0+gap,a.y+size.y*0.33f),ImVec2(x1-gap,b.y-size.y*0.33f),Col(leds[static_cast<size_t>(i)]),S());
    }
    for(float x:{a.x+end*0.5f,b.x-end*0.5f})
        dl->AddCircleFilled(ImVec2(x,a.y+size.y*0.5f),S(),Hex(0xA5B0C1),10);
}

// A generic graphics card from above, with a metal shroud, two cooling fans and edge lighting.
void DrawGpu2d(ImDrawList* dl, ImVec2 a, ImVec2 size, const std::vector<Rgb>& leds) {
    const float w=size.x,h=size.y;
    auto p=[&](float x,float y){return ImVec2(a.x+x*w,a.y+y*h);};
    dl->AddRectFilled(p(0.05f,0.04f),p(0.98f,0.89f),Hex(0x283241),4*S());
    dl->AddRect(p(0.05f,0.04f),p(0.98f,0.89f),Hex(0x68748A),4*S(),0,S());
    dl->AddRectFilled(p(0.00f,0.00f),p(0.055f,0.94f),Hex(0x8A95A6),S());
    for(int i=0;i<5;++i)dl->AddLine(p(0.008f,0.16f+i*0.12f),p(0.045f,0.16f+i*0.12f),Hex(0x323B49),S());
    for(float x:{0.32f,0.74f}) {
        const ImVec2 center=p(x,0.45f);
        const float r=h*0.31f;
        dl->AddCircleFilled(center,r,Hex(0x0A1018),36);
        dl->AddCircle(center,r,Hex(0x546176),36,S());
        for(int blade=0;blade<9;++blade){
            const float angle=6.2831853f*blade/9;
            dl->AddLine(ImVec2(center.x+std::cos(angle)*r*0.25f,center.y+std::sin(angle)*r*0.25f),
                        ImVec2(center.x+std::cos(angle+0.35f)*r*0.80f,center.y+std::sin(angle+0.35f)*r*0.80f),Hex(0x39465A),2*S());
        }
        dl->AddCircleFilled(center,r*0.25f,Hex(0x687489),20);
    }
    dl->AddRectFilled(p(0.30f,0.89f),p(0.67f,0.96f),Hex(0xA68A49),S());
    for(int i=0;i<12;++i)dl->AddLine(p(0.31f+i*0.03f,0.91f),p(0.31f+i*0.03f,0.96f),Hex(0x4D422C),0.7f*S());
    const int count=static_cast<int>(leds.size());
    for(int i=0;i<count;++i){
        const float x0=0.13f+0.79f*i/count,x1=0.13f+0.79f*(i+1)/count;
        dl->AddLine(p(x0,0.80f),p(x1,0.80f),Col(leds[static_cast<size_t>(i)],40),4*S());
        dl->AddLine(p(x0,0.80f),p(x1,0.80f),Col(leds[static_cast<size_t>(i)]),1.8f*S());
    }
}

// A padded headband, adjustable forks and ear cushions with light strips on the outer cups.
void DrawHeadset(ImDrawList* dl, ImVec2 a, ImVec2 size, const std::vector<Rgb>& leds) {
    const float w=size.x,h=size.y;
    auto p=[&](float x,float y){return ImVec2(a.x+x*w,a.y+y*h);};
    auto band=[&]{
        dl->PathLineTo(p(0.18f,0.60f));
        dl->PathBezierCubicCurveTo(p(0.16f,0.12f),p(0.31f,0.06f),p(0.50f,0.06f));
        dl->PathBezierCubicCurveTo(p(0.69f,0.06f),p(0.84f,0.12f),p(0.82f,0.60f));
    };
    band();dl->PathStroke(Hex(0x626F83),ImDrawFlags_None,w*0.09f);
    band();dl->PathStroke(Hex(0x222D3D),ImDrawFlags_None,w*0.065f);
    for(int side=0;side<2;++side){
        const float x=side?0.80f:0.20f;
        dl->AddLine(p(x,0.39f),p(x,0.57f),Hex(0xA0ADBF),2*S());
        dl->AddRectFilled(p(x-0.12f,0.51f),p(x+0.12f,0.87f),Hex(0x4B586E),w*0.09f);
        dl->AddRectFilled(p(x-0.075f,0.55f),p(x+0.075f,0.84f),Hex(0x111824),w*0.06f);
        dl->AddRect(p(x-0.06f,0.575f),p(x+0.06f,0.815f),Hex(0x344156),w*0.045f,0,S());
        const Rgb color=static_cast<size_t>(side)<leds.size()?leds[static_cast<size_t>(side)]:Rgb{50,54,64};
        const float edge=x+(side?0.10f:-0.10f);
        dl->AddLine(p(edge,0.60f),p(edge,0.78f),Col(color,40),5*S());
        dl->AddLine(p(edge,0.60f),p(edge,0.78f),Col(color),2*S());
    }
    dl->PathLineTo(p(0.12f,0.79f));
    dl->PathBezierCubicCurveTo(p(0.08f,0.94f),p(0.23f,0.95f),p(0.39f,0.93f));
    dl->PathStroke(Hex(0x8C9AAF),ImDrawFlags_None,1.6f*S());
    dl->AddRectFilled(p(0.36f,0.90f),p(0.44f,0.95f),Hex(0x364258),S());
}

// DualSense from above: white grip shells around the dark center, with the game's color
// confined to the two strips beside the touchpad. Buttons stay their physical colors, except
// with `live` (the controller page): held buttons turn the accent color, the stick caps move
// with the sticks, and fingers show on the touchpad.
void DrawController(ImDrawList* dl, ImVec2 a, ImVec2 size, Rgb c, bool lit, const pad::State* live = nullptr) {
    const float w = size.x, h = size.y;
    auto held = [&](int b) { return live && live->Down(b); };
    auto axis = [](uint8_t v) { return std::clamp((static_cast<float>(v) - 128.f) / 127.f, -1.f, 1.f); };
    auto p = [&](float x, float y) { return ImVec2(a.x + x * w, a.y + y * h); };
    const float line = std::max(0.8f * S(), w * 0.006f);
    auto silhouette = [&] {
        dl->PathLineTo(p(0.23f, 0.10f));
        dl->PathBezierCubicCurveTo(p(0.38f, 0.06f), p(0.62f, 0.06f), p(0.77f, 0.10f));
        dl->PathBezierCubicCurveTo(p(0.89f, 0.10f), p(0.93f, 0.22f), p(0.96f, 0.46f));
        dl->PathBezierCubicCurveTo(p(0.98f, 0.60f), p(1.00f, 0.90f), p(0.89f, 0.95f));
        dl->PathBezierCubicCurveTo(p(0.79f, 1.00f), p(0.73f, 0.83f), p(0.66f, 0.69f));
        dl->PathBezierCubicCurveTo(p(0.59f, 0.79f), p(0.41f, 0.79f), p(0.34f, 0.69f));
        dl->PathBezierCubicCurveTo(p(0.27f, 0.83f), p(0.21f, 1.00f), p(0.11f, 0.95f));
        dl->PathBezierCubicCurveTo(p(0.00f, 0.90f), p(0.02f, 0.60f), p(0.04f, 0.46f));
        dl->PathBezierCubicCurveTo(p(0.07f, 0.22f), p(0.11f, 0.10f), p(0.23f, 0.10f));
    };
    // The shoulder buttons sit behind the shell, visible along its top edge.
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 0.75f : 0.10f;
        const bool on = held(side ? pad::kR1 : pad::kL1);
        dl->AddRectFilled(p(x, 0.04f), p(x + 0.15f, 0.17f), Hex(on ? kAccent : 0x181C24), h * 0.045f);
        dl->AddLine(p(x + 0.025f, 0.075f), p(x + 0.125f, 0.075f), Hex(on ? 0xFFFFFF : 0x4B5362), line);
    }
    silhouette();
    dl->PathFillConcave(Hex(0xD3D7DE));
    silhouette();
    dl->PathStroke(Hex(0x929BAA), ImDrawFlags_Closed, line);
    // White handles taper around a recessed black panel and the two analog sticks.
    dl->PathLineTo(p(0.30f, 0.10f));
    dl->PathBezierCubicCurveTo(p(0.43f, 0.07f), p(0.57f, 0.07f), p(0.70f, 0.10f));
    dl->PathLineTo(p(0.72f, 0.43f));
    dl->PathBezierCubicCurveTo(p(0.75f, 0.60f), p(0.72f, 0.67f), p(0.65f, 0.69f));
    dl->PathBezierCubicCurveTo(p(0.59f, 0.77f), p(0.41f, 0.77f), p(0.35f, 0.69f));
    dl->PathBezierCubicCurveTo(p(0.28f, 0.67f), p(0.25f, 0.60f), p(0.28f, 0.43f));
    dl->PathFillConcave(Hex(0x171B23));
    for (int side = 0; side < 2; ++side) {
        const float sign = side ? 1.f : -1.f;
        // A subtle seam follows the inside of each grip.
        dl->PathLineTo(p(0.5f + sign * 0.28f, 0.63f));
        dl->PathBezierCubicCurveTo(p(0.5f + sign * 0.29f, 0.76f), p(0.5f + sign * 0.32f, 0.85f),
                                  p(0.5f + sign * 0.36f, 0.89f));
        dl->PathStroke(Hex(0xB2BAC7), ImDrawFlags_None, line);
    }
    // The broad touchpad has a sloped lower edge, with long light strips on either side.
    const ImVec2 pad[] = {p(0.33f, 0.14f), p(0.67f, 0.14f), p(0.68f, 0.36f),
                          p(0.64f, 0.42f), p(0.36f, 0.42f), p(0.32f, 0.36f)};
    const bool padDown = held(pad::kTouchpad);
    dl->AddConvexPolyFilled(pad, 6, Hex(padDown ? 0x3D3878 : 0x303640));
    dl->AddPolyline(pad, 6, Hex(padDown ? kAccentHover : 0x4B5360), ImDrawFlags_Closed, padDown ? 2 * line : line);
    if (live)
        for (int i = 0; i < 2; ++i) {
            const pad::Touch& t = live->touch[i];
            if (!t.down) continue;
            const ImVec2 at = p(0.34f + 0.32f * std::clamp(t.x / 1919.f, 0.f, 1.f), 0.15f + 0.26f * std::clamp(t.y / 1079.f, 0.f, 1.f));
            dl->AddCircleFilled(at, w * 0.022f, Hex(i ? kAccent2 : kAccent, 70), 20);
            dl->AddCircleFilled(at, w * 0.011f, Hex(i ? kAccent2 : kAccent), 16);
        }
    for (int side = 0; side < 2; ++side) {
        const float sign = side ? 1.f : -1.f;
        const ImVec2 strip[] = {p(0.5f + sign * 0.185f, 0.16f), p(0.5f + sign * 0.195f, 0.36f),
                                p(0.5f + sign * 0.15f, 0.425f)};
        if (lit) {
            dl->AddPolyline(strip, 3, Col(c, 22), ImDrawFlags_None, 7 * line);
            dl->AddPolyline(strip, 3, Col(c, 55), ImDrawFlags_None, 4 * line);
        }
        dl->AddPolyline(strip, 3, lit ? Col(c) : Hex(0x4C5564), ImDrawFlags_None, 1.8f * line);
    }
    // Raised analog sticks: a shaded socket, rubber cap and fine rim.
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 0.625f : 0.375f;
        const ImVec2 center = p(x, 0.59f);
        dl->AddCircleFilled(center, w * 0.081f, Hex(0x080B10), 28);
        // The cap follows the stick inside its socket.
        const float sx = live ? axis(side ? live->rx : live->lx) * w * 0.022f : 0.f;
        const float sy = live ? axis(side ? live->ry : live->ly) * w * 0.022f : 0.f;
        const ImVec2 cap(center.x + sx, p(x, 0.582f).y + sy);
        const bool click = held(side ? pad::kR3 : pad::kL3);
        dl->AddCircleFilled(cap, w * 0.064f, Hex(click ? kAccent : 0x353B46), 28);
        dl->AddCircle(cap, w * 0.064f, Hex(click ? kAccentHover : 0x606978), 28, line);
        dl->AddCircleFilled(cap, w * 0.048f, Hex(click ? 0x6A5CE0 : 0x252B35), 28);
        dl->PathArcTo(cap, w * 0.054f, 3.8f, 5.6f, 12);
        dl->PathStroke(Hex(0x48515E), ImDrawFlags_None, line * 0.7f);
    }
    // Four separate D-pad directions, rather than a single cross stamped on the shell.
    const ImVec2 dpad[] = {p(0.19f, 0.245f), p(0.21f, 0.23f), p(0.23f, 0.245f),
                           p(0.23f, 0.30f), p(0.21f, 0.325f), p(0.19f, 0.30f)};
    const ImVec2 dc = p(0.21f, 0.36f);
    for (int direction = 0; direction < 4; ++direction) {
        ImVec2 shape[6];
        for (int i = 0; i < 6; ++i) {
            const float x = dpad[i].x - dc.x, y = dpad[i].y - dc.y;
            shape[i] = direction == 0 ? ImVec2(dc.x + x, dc.y + y)
                     : direction == 1 ? ImVec2(dc.x - y, dc.y + x)
                     : direction == 2 ? ImVec2(dc.x - x, dc.y - y) : ImVec2(dc.x + y, dc.y - x);
        }
        static const int arms[4] = {pad::kUp, pad::kRight, pad::kDown, pad::kLeft};
        const bool on = held(arms[direction]);
        dl->AddConvexPolyFilled(shape, 6, Hex(on ? kAccent : 0x343A45));
        dl->AddPolyline(shape, 6, Hex(on ? kAccentHover : 0x697281), ImDrawFlags_Closed, line * 0.7f);
    }
    // Face-button symbols remain neutral, like the transparent buttons on a DualSense.
    const ImVec2 buttons[] = {p(0.79f, 0.26f), p(0.855f, 0.36f), p(0.79f, 0.46f), p(0.725f, 0.36f)};
    const float radius = w * 0.041f, mark = radius * 0.43f;
    for (int i = 0; i < 4; ++i) {
        const ImVec2 center = buttons[i];
        static const int faces[4] = {pad::kTriangle, pad::kCircle, pad::kCross, pad::kSquare};
        const bool on = held(faces[i]);
        dl->AddCircleFilled(center, radius, Hex(on ? kAccent : 0xADB5C2), 20);
        dl->AddCircle(center, radius, Hex(on ? kAccentHover : 0x8893A3), 20, line * 0.7f);
        const ImU32 ink = Hex(on ? 0xFFFFFF : 0x4E596A);
        if (i == 0) {
            const ImVec2 tri[] = {ImVec2(center.x, center.y - mark), ImVec2(center.x + mark, center.y + mark),
                                  ImVec2(center.x - mark, center.y + mark)};
            dl->AddPolyline(tri, 3, ink, ImDrawFlags_Closed, line * 0.7f);
        } else if (i == 1) dl->AddCircle(center, mark, ink, 16, line * 0.7f);
        else if (i == 2) {
            dl->AddLine(ImVec2(center.x - mark, center.y - mark), ImVec2(center.x + mark, center.y + mark), ink, line * 0.7f);
            dl->AddLine(ImVec2(center.x - mark, center.y + mark), ImVec2(center.x + mark, center.y - mark), ink, line * 0.7f);
        } else dl->AddRect(ImVec2(center.x - mark, center.y - mark), ImVec2(center.x + mark, center.y + mark), ink, 0, 0, line * 0.7f);
    }
    // Create / options buttons, speaker holes, center button and microphone mute button.
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 0.725f : 0.275f;
        dl->AddRectFilled(p(x - 0.009f, 0.185f), p(x + 0.009f, 0.24f), Hex(held(side ? pad::kOptions : pad::kCreate) ? kAccent : 0x6A7483), line);
    }
    for (int row = 0; row < 2; ++row)
        for (int i = 0; i < 5; ++i) dl->AddCircleFilled(p(0.46f + i * 0.02f, 0.46f + row * 0.025f), line * 0.45f, Hex(0x6B7481), 8);
    dl->AddCircleFilled(p(0.5f, 0.59f), w * 0.018f, Hex(held(pad::kPs) ? kAccent : 0x4C5564), 16);
    dl->AddRectFilled(p(0.475f, 0.67f), p(0.525f, 0.69f), Hex(held(pad::kMute) ? kAmber : 0x616A78), line);
}

// The canvas. `selectable`: clicking a device selects it for editing (Manual mode).
// The memory slots drawn filled: as set by hand, else as the system scan says, else a guess.

void SetupCanvas2d(Controller& ctl, UiState& ui, bool selectable, bool overview) {
    Prefs& prefs = ctl.prefs();
    const float W = ImGui::GetContentRegionAvail().x;
    float H = std::clamp(W * 0.52f, 300 * S(), 480 * S());
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // What's there: the same things the 3D view shows (one model for both), flat.
    const view3d::Model model = view3d::Gather(ctl, ctl.monitor().Snapshot());
    const fx::FanLayout& layout = ctl.config().argbFans;
    std::vector<SetupItem> items;
    if (model.fansRgb || overview)
        for (int i = 0; i < (model.fansRgb ? layout.Fans() : model.layout.Fans()); ++i)
            items.push_back({FanItem(i), model.fansRgb ? device::kFans : nullptr, ImVec2(84 * S(), 84 * S()), i, ""});
    if (model.boardRgb || model.ramRgb || !ctl.devices().empty() || HasRgbRam(ctl, ctl.monitor().Snapshot()) ||
        (overview && !model.boardName.empty()))
        items.push_back({device::kBoard, overview && !model.boardRgb ? nullptr : device::kBoard,
                         ImVec2(240 * S(), 180 * S()), -1, ""});
    if (model.gpuRgb || (overview && !model.gpuName.empty()))
        items.push_back({"gpu", model.gpuRgb ? device::kOther : nullptr, ImVec2(148 * S(), 68 * S()), -1, model.gpuName, SetupItem::Look::Gpu, 10});
    // The board and memory as the system scan found them. The memory is drawn in the board's
    // slots (and selected by clicking the sticks); lit only when RAM lighting is on.
    const SetupHardware hw = DetectSetup(ctl.monitor().Snapshot().smbios);
    const bool ramOn = prefs.ramLighting;
    const std::array<bool, 4> slots = RamSlotsShown(ctl, hw);
    // The desk: keyboard and mouse (plain ones too, as in 3D), headset, controller, the rest as bars.
    items.push_back({device::kKeyboard, model.keyboard.device, ImVec2(330 * S(), 138 * S()), -1, model.keyboard.name});
    items.push_back({device::kMouse, model.mouse.device, ImVec2(70 * S(), 112 * S()), -1, model.mouse.name});
    if (model.headset)
        items.push_back({"headset", model.headsetGear.device, ImVec2(92 * S(), 88 * S()), -1, model.headsetGear.name,
                         SetupItem::Look::Headset, 8});
    if (model.controller)
        items.push_back({"controller", model.controllerGear.device, ImVec2(132 * S(), 84 * S()), -1, model.controllerGear.name,
                         SetupItem::Look::Controller, 1});
    if (overview && model.aio)
        items.push_back({"cooler", nullptr, ImVec2(150 * S(), 70 * S()), -1, model.aio->name, SetupItem::Look::Cooler});
    for (size_t i = 0; i < model.others.size(); ++i)
        items.push_back({"other" + std::to_string(i), model.others[i].device, ImVec2(120 * S(), 26 * S()), -1, model.others[i].name,
                         SetupItem::Look::Bar, model.others[i].leds});

    // Auto uses a fitted overview rather than saved drag positions, so every device stays visible.
    const int columns = std::max(1, static_cast<int>(W / (160 * S())));
    const float cellW = W / columns, cellH = 124 * S();
    if (overview) {
        H = std::max(350 * S(), std::max(1, (static_cast<int>(items.size()) + columns - 1) / columns) * cellH + 12 * S());
        for (auto& it : items) {
            const float fit = std::min({1.f, (cellW - 24 * S()) / it.size.x, (cellH - 40 * S()) / it.size.y});
            it.size.x *= fit;
            it.size.y *= fit;
        }
    }
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), Hex(0x0B0E14), 12 * S());
    if (!overview)
        for (float x = o.x + 20 * S(); x < o.x + W; x += 24 * S())
            for (float y = o.y + 20 * S(); y < o.y + H; y += 24 * S()) dl->AddCircleFilled(ImVec2(x, y), 1 * S(), Hex(0x1C2230), 4);

    // Live colors.
    const double t = ImGui::GetTime();
    std::vector<Rgb> fanLeds, boardLeds, mouseLeds(8);
    const fx::Params* fanP = model.fansRgb ? LiveParams(ctl, device::kFans) : nullptr;
    if (fanP) {
        fx::RenderFans(*fanP, t, layout, &fanLeds);
        const double level = LiveLevel(ctl, device::kFans);
        for (Rgb& c : fanLeds) c = Scale(c, level);
    } else {
        fanLeds.assign(static_cast<size_t>(layout.TotalLeds()), Rgb{50, 54, 64});
    }
    const fx::Params* boardP = model.boardRgb ? LiveParams(ctl, device::kBoard) : nullptr;
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
    if (!overview)
        std::stable_sort(items.begin(), items.end(), [&](const SetupItem& a, const SetupItem& b) {
            return (a.item == ui.dragItem2d) < (b.item == ui.dragItem2d);
        });
    int index = 0;
    ImGui::PushID("setup");
    for (const SetupItem& it : items) {
        const int cell = index++;
        const ImVec2 a = overview ? ImVec2(o.x + (cell % columns) * cellW + (cellW - it.size.x) / 2,
                                         o.y + 8 * S() + (cell / columns) * cellH + (cellH - 36 * S() - it.size.y) / 2)
                                 : rectOf(it);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton(it.item.c_str(), it.size);
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        if (!overview && active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3 * S())) {
            ui.dragItem2d = it.item;
            Spot s = SetupSpot(prefs, it.item);
            // From the item's clamped place, so a drag never starts with a jump.
            s.x = (a.x + it.size.x / 2 - o.x + ImGui::GetIO().MouseDelta.x) / W;
            s.y = (a.y + it.size.y / 2 - o.y + ImGui::GetIO().MouseDelta.y) / H;
            prefs.setupSpots[it.item] = ClampSpot(s);
        }
        // On the board, the memory sticks are their own device.
        ImVec2 ra, rb;
        RamArea(a, it.size, &ra, &rb);
        const bool isBoard = it.item == device::kBoard;
        auto inRam = [&](ImVec2 m) { return isBoard && ramOn && m.x >= ra.x && m.x <= rb.x && m.y >= ra.y && m.y <= rb.y; };
        const char* target = inRam(ImGui::GetIO().MouseClickedPos[0]) ? device::kRam : it.device;  // nullptr: not lit, can't be edited
        if (ImGui::IsItemDeactivated()) {
            if (!overview && ui.dragItem2d == it.item) {
                ui.dragItem2d.clear();
                ctl.Changed();  // save the layout
            } else if (selectable && target) {
                ui.lightTarget = ui.lightTarget == target ? "" : target;
            }
        }
        const bool hoverRam = hovered && inRam(ImGui::GetIO().MousePos);
        if (!overview && (hovered || active)) ImGui::SetMouseCursor(active ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);

        const ImVec2 b(a.x + it.size.x, a.y + it.size.y);
        auto isDev = [&](const char* id) { return it.device && std::strcmp(it.device, id) == 0; };
        if (it.look == SetupItem::Look::Bar) DrawBar(dl, a, it.size, Leds2d(ctl, it.device, t, it.leds));
        else if (it.look == SetupItem::Look::Gpu) DrawGpu2d(dl, a, it.size, Leds2d(ctl, it.device, t, it.leds));
        else if (it.look == SetupItem::Look::Headset) DrawHeadset(dl, a, it.size, Leds2d(ctl, it.device, t, 2));
        else if (it.look == SetupItem::Look::Controller) DrawController(dl, a, it.size, Leds2d(ctl, it.device, t, 1)[0], it.device && LiveParams(ctl, it.device));
        else if (it.look == SetupItem::Look::Cooler) {
            const ImVec2 radiator(a.x, a.y + it.size.y * 0.1f);
            const float side = it.size.y * 0.7f;
            const std::vector<Rgb> dark(12, Rgb{50, 54, 64});
            DrawFan(dl, radiator, ImVec2(side, side), dark, 0, 12);
            DrawFan(dl, ImVec2(radiator.x + side + 2 * S(), radiator.y), ImVec2(side, side), dark, 0, 12);
            const ImVec2 pump(a.x + it.size.x - side * 0.35f, a.y + it.size.y * 0.65f);
            dl->AddBezierCubic(ImVec2(radiator.x + side, radiator.y + side),
                               ImVec2(radiator.x + side, a.y + it.size.y), ImVec2(pump.x, a.y + it.size.y),
                               pump, Hex(0x697384), 3 * S());
            dl->AddCircleFilled(pump, side * 0.35f, Hex(0x353C4B), 24);
            dl->AddCircleFilled(pump, side * 0.27f, Hex(kTrack), 24);
            dl->AddCircle(pump, side * 0.35f, Hex(0x697384), 24, S());
        }
        else if (it.fan >= 0) DrawFan(dl, a, it.size, fanLeds, it.fan, layout.LedsPerFan());
        else if (it.item == device::kBoard)
            DrawBoard(dl, a, it.size, boardLeds, hw, slots, ramOn, LiveParams(ctl, device::kRam), t, LiveLevel(ctl, device::kRam));
        else if (it.item == device::kMouse) {
            // Another device's mouse (OpenRGB, ...) shows one color.
            if (isDev(device::kMouse)) DrawMouse(dl, a, it.size, mouseLeds);
            else DrawMouse(dl, a, it.size, Leds2d(ctl, it.device, t, 8, true));
        } else if (it.item == device::kKeyboard) {
            // Key by key, as the keyboard shows it; one color for any other keyboard.
            const fx::Params* kp = it.device ? LiveParams(ctl, it.device) : nullptr;
            const double level = it.device ? LiveLevel(ctl, it.device) : 0;
            std::vector<Rgb> keys;
            if (kp && isDev(device::kKeyboard)) keys = azoth::RenderKeys(*kp, t, level);
            else keys.assign(azoth::IsoKeys().size(), kp ? LiveAt(kp, t, 0, 1, level) : Rgb{50, 54, 64});
            DrawKeyboard(dl, a, it.size, keys);
        }

        auto own = [&](const char* id) {
            auto d = prefs.deviceLighting.find(id);
            return d != prefs.deviceLighting.end() && d->second.own;
        };
        const bool selected = selectable && it.device && ui.lightTarget == it.device;
        if (selected || (hovered && !hoverRam))
            dl->AddRect(ImVec2(a.x - 4 * S(), a.y - 4 * S()), ImVec2(b.x + 4 * S(), b.y + 4 * S()),
                        Hex(selected ? kAccentHover : kBorder), 10 * S(), 0, (selected ? 2.f : 1.2f) * S());
        if (isBoard && ramOn) {
            // The memory's outline and name, inside the board.
            const bool ramSelected = selectable && ui.lightTarget == device::kRam;
            if (ramSelected || hoverRam)
                dl->AddRect(ImVec2(ra.x - 5 * S(), ra.y - 4 * S()), ImVec2(rb.x + 5 * S(), rb.y + 4 * S()),
                            Hex(ramSelected ? kAccentHover : kBorder), 6 * S(), 0, (ramSelected ? 2.f : 1.2f) * S());
            if (!overview) {
                const char* name = hw.ramName.empty() ? device::Name(device::kRam) : hw.ramName.c_str();
                const ImVec2 ns = ImGui::CalcTextSize(name);
                const ImVec2 np((ra.x + rb.x) / 2 - ns.x / 2, rb.y + 6 * S());
                dl->AddText(np, Hex(ramSelected ? kText : kMuted), name);
                if (own(device::kRam))
                    dl->AddCircleFilled(ImVec2(np.x + ns.x + 7 * S(), np.y + ns.y / 2), 3 * S(), Hex(kAccentHover), 12);
            }
        }
        // The name under it; fans are numbered.
        char label[96];
        if (it.fan >= 0) snprintf(label, sizeof label, "Fan %d", it.fan + 1);
        else if (!it.name.empty()) snprintf(label, sizeof label, "%s", it.name.c_str());
        else if (isBoard && !hw.boardName.empty()) snprintf(label, sizeof label, "%s", hw.boardName.c_str());
        else snprintf(label, sizeof label, "%s", it.device ? device::Name(it.device) : "");
        if (overview) {
            if (hovered) ImGui::SetTooltip("%s%s%s", label, isBoard && !hw.ramName.empty() ? "\n" : "",
                                          isBoard ? hw.ramName.c_str() : "");
            if (isBoard) snprintf(label, sizeof label, "%s", hw.ramName.empty() ? "Motherboard" : "Motherboard + RAM");
            std::string clipped = label;
            if (ImGui::CalcTextSize(clipped.c_str()).x > cellW - 20 * S()) {
                while (!clipped.empty() && ImGui::CalcTextSize((clipped + "...").c_str()).x > cellW - 20 * S()) {
                    // Remove a complete UTF-8 character before appending the ellipsis.
                    size_t last = clipped.size() - 1;
                    while (last > 0 && (static_cast<unsigned char>(clipped[last]) & 0xC0) == 0x80) --last;
                    clipped.erase(last);
                }
                clipped += "...";
            }
            snprintf(label, sizeof label, "%s", clipped.c_str());
        }
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(a.x + it.size.x / 2 - ts.x / 2,
                          overview ? o.y + 8 * S() + (cell / columns) * cellH + cellH - 30 * S() : b.y + 3 * S()),
                    Hex(selected ? kText : kMuted), label);
        if (!overview && it.device && own(it.device)) dl->AddCircleFilled(ImVec2(a.x + it.size.x / 2 + ts.x / 2 + 7 * S(), b.y + 3 * S() + ts.y / 2), 3 * S(),
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

// The setup in 3D or 2D, on the Lighting page and in the
// setup guide. `selectable`: clicking a lit part selects it for editing (Manual mode).
void SetupCanvas(Controller& ctl, UiState& ui, bool selectable) {
    if (!ctl.prefs().lighting3d) {
        SetupCanvas2d(ctl, ui, selectable);
        return;
    }
    const float h = std::clamp(ImGui::GetContentRegionAvail().x * 0.5f, 320 * S(), 500 * S());
    SetupView(ctl, ui, selectable ? view3d::Mode::Lighting : view3d::Mode::Preview, h);
}

void ManualOverview(Controller& ctl, UiState& ui, const Fonts& f) {
    const bool side = ImGui::GetContentRegionAvail().x >= 760 * S();
    if (ImGui::BeginTable("manual-overview", side ? 2 : 1, ImGuiTableFlags_SizingStretchProp)) {
        if (side) {
            ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 280 * S());
            ImGui::TableSetupColumn("Setup", ImGuiTableColumnFlags_WidthStretch);
        }
        const bool native = !ui.lightTarget.empty() && DeviceNative(ctl.prefs(), ui.lightTarget);
        ImGui::TableNextColumn();
        ManualColorCard(ctl, ui, f);
        if (side && !native) BrightnessCard(ctl, f, ui.lightTarget);
        ImGui::TableNextColumn();
        BeginCard("manual-setup");
        const ImVec2 top = ImGui::GetCursorPos();
        CardTitle(f, "Your setup", Icon::Grid);
        const ImVec2 below = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - 120 * S(), top.y - 4 * S()));
        int view = ctl.prefs().lighting3d ? 1 : 0;
        const char* views[] = {"2D", "3D"};
        if (Segmented("view2d3d", &view, views, 2, 120 * S())) {
            ctl.prefs().lighting3d = view == 1;
            ctl.Changed();
        }
        ImGui::SetCursorPos(below);
        Muted(ctl.prefs().lighting3d ? "Click a lit component to edit its lighting."
                                    : "Drag devices to arrange your setup. Click a lit component to edit its lighting.");
        ImGui::Dummy(ImVec2(0, 6 * S()));
        if (ctl.prefs().lighting3d) SetupCanvas(ctl, ui, true);
        else SetupCanvas2d(ctl, ui, true);
        if (ctl.prefs().lighting3d) {
            if (SmallBtn("Reset view")) ui.lightView.camSet = false;
        } else if (SmallBtn("Reset layout")) {
            auto& spots = ctl.prefs().setupSpots;
            for (auto it = spots.begin(); it != spots.end();)
                it = it->first.rfind("desk:", 0) == 0 ? std::next(it) : spots.erase(it);
            ctl.Changed();
        }
        EndCard();
        if (!side && !native) BrightnessCard(ctl, f, ui.lightTarget);
        ImGui::EndTable();
    }
}

// ---- My setup: the PC and the desk in 3D --------------------------------------------------

// Saves a corrected case layout.
void SaveLayout(Controller& ctl, pc::Layout l) {
    l.guessed = false;
    ctl.prefs().caseLayout = pc::Encode(l);
    ctl.Changed();
}

void RgbPill(bool rgb) { Pill(rgb ? "RGB" : "No RGB", rgb ? kGreen : kMuted); }

void MySetupPage(Controller& ctl, UiState& ui, const Fonts& f) {
    const sensors::SystemSnapshot snap = ctl.monitor().Snapshot();
    const view3d::Model m = view3d::Gather(ctl, snap);

    // Tools above the view.
    Toggle("Airflow", &ui.setupAirflow);
    ImGui::SameLine(0, 22 * S());
    Toggle("Names", &ui.setupLabels);
    ImGui::SameLine(0, 22 * S());
    if (ui.setupEdit ? PrimaryButton("Done editing fans") : Btn("Edit fans")) ui.setupEdit = !ui.setupEdit;
    ImGui::SameLine();
    if (Btn("Turn the case")) {
        pc::Layout l = m.layout;
        l.turn = (l.turn + 1) % 4;
        SaveLayout(ctl, l);
    }
    ImGui::SameLine();
    if (Btn("Reset view")) ui.setupView.camSet = false;
    ImGui::SameLine();
    if (Btn("Put everything back")) {
        for (const char* item : pc::DeskItems()) ctl.prefs().setupSpots.erase(item);
        ctl.Changed();
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    const float h = std::clamp(ImGui::GetContentRegionAvail().x * 0.56f, 380 * S(), 680 * S());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * S());
    ImGui::BeginChild("view", ImVec2(0, h + 6 * S()), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    SetupView(ctl, ui, view3d::Mode::MySetup, h);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, 8 * S()));

    const float avail = ImGui::GetContentRegionAvail().x;
    if (!ImGui::BeginTable("mysetup", avail > 820 * S() ? 2 : 1, ImGuiTableFlags_SizingStretchSame)) return;
    ImGui::TableNextColumn();

    // The fans: what LumaBridge guessed, and correcting it.
    BeginCard("fans3d");
    CardTitle(f, "Fans and cooling", Icon::Fan);
    const pc::Layout& l = m.layout;
    if (l.guessed)
        Muted("LumaBridge's guess: %d fan%s, %d of them RGB on the ARGB header. Wrong? Click Edit fans, then click the "
              "slots in the view: empty, fan, RGB fan.",
              l.Fans(), l.Fans() == 1 ? "" : "s", l.RgbFans());
    else
        Muted("Set by you: %d fan%s, %d of them RGB. Click Edit fans to change them.", l.Fans(), l.Fans() == 1 ? "" : "s", l.RgbFans());
    ImGui::Dummy(ImVec2(0, 4 * S()));
    // Each position: how many fans (the - / + change it), and which way they blow.
    for (int mount = 0; mount < pc::kMounts; ++mount) {
        const pc::Mount mt = static_cast<pc::Mount>(mount);
        const int fans = pc::FansAt(l, mt), slots = pc::SlotsAt(mt);
        ImGui::PushID(mount);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(pc::MountName(mt));
        ImGui::SameLine(80 * S());
        ImGui::BeginDisabled(fans == 0);
        if (Btn("-", ImVec2(28 * S(), 0))) {
            pc::Layout c = l;
            pc::SetFansAt(&c, mt, fans - 1);
            SaveLayout(ctl, c);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d", fans);
        ImGui::SameLine();
        ImGui::BeginDisabled(fans >= slots);
        if (Btn("+", ImVec2(28 * S(), 0))) {
            pc::Layout c = l;
            pc::SetFansAt(&c, mt, fans + 1);
            SaveLayout(ctl, c);
        }
        ImGui::EndDisabled();
        if (fans) {
            ImGui::SameLine(190 * S());
            int dir = l.exhaust[static_cast<size_t>(mount)] ? 1 : 0;
            const char* dirs[] = {"Pulls air in", "Blows out"};
            if (Segmented("dir", &dir, dirs, 2, 240 * S())) {
                pc::Layout c = l;
                c.exhaust[static_cast<size_t>(mount)] = dir == 1;
                SaveLayout(ctl, c);
            }
        } else {
            ImGui::Dummy(ImVec2(0, 2 * S()));
        }
        ImGui::PopID();
    }
    Muted("Which of them are RGB fans (on the ARGB header): Edit fans, then click a fan.");
    // The CPU cooler.
    ImGui::Dummy(ImVec2(0, 2 * S()));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("CPU cooler");
    ImGui::SameLine(110 * S());
    int cooler = l.cooler == pc::Cooler::Aio ? 1 : 0;
    const char* coolers[] = {"Air (fan)", "AIO (water)"};
    if (Segmented("cooler", &cooler, coolers, 2, 260 * S())) {
        pc::Layout c = l;
        c.cooler = cooler ? pc::Cooler::Aio : pc::Cooler::Air;
        SaveLayout(ctl, c);
    }
    if (m.aio) {
        Muted("Found on USB: %s%s.", m.aio->name, m.aio->lcd ? " (with a screen)" : "");
        if (l.cooler != pc::Cooler::Aio) {
            ImGui::SameLine();
            if (SmallBtn("Show it")) {
                pc::Layout c = l;
                c.cooler = pc::Cooler::Aio;
                SaveLayout(ctl, c);
            }
        }
        if (m.kraken.valid)
            Muted("Read from the Kraken: liquid %.1f \xC2\xB0" "C, pump %d RPM%s.", m.kraken.liquidC, m.kraken.pumpRpm,
                  m.kraken.fanRpm > 0 ? (", fans " + std::to_string(m.kraken.fanRpm) + " RPM").c_str() : "");
        else if (m.aio->vid == nzxt::kVid) {
            switch (ctl.krakenState()) {
                case nzxt::KrakenState::CantOpen: Muted("Windows won't let LumaBridge open the Kraken (details in the log)."); break;
                case nzxt::KrakenState::NoReply: Muted("The Kraken isn't answering status requests yet; still trying."); break;
                case nzxt::KrakenState::Searching: Muted("Looking for the Kraken's USB interface..."); break;
                default: Muted("Waiting for its first status reading..."); break;
            }
        }
    } else {
        // Nothing known: list what NZXT has on USB, so a new model can be added.
        std::string nzxtIds;
        char b[16];
        for (const auto& [vid, pid] : ctl.presence().usb)
            if (vid == nzxt::kVid) snprintf(b, sizeof b, "%s%04X", nzxtIds.empty() ? "" : ", ", pid), nzxtIds += b;
        if (!nzxtIds.empty()) Muted("NZXT on USB (product %s): not a cooler LumaBridge knows yet.", nzxtIds.c_str());
    }
    if (l.cooler == pc::Cooler::Aio) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Radiator");
        ImGui::SameLine(110 * S());
        int where = l.radiator == pc::Mount::Front ? 1 : 0;
        const char* places[] = {"Top", "Front"};
        if (Segmented("radiator", &where, places, 2, 260 * S())) {
            pc::Layout c = l;
            c.radiator = where ? pc::Mount::Front : pc::Mount::Top;
            SaveLayout(ctl, c);
        }
        Muted("The radiator sits behind the fans there.");
        bool pumpRgb = l.pumpRgb;
        if (Toggle("The pump head has RGB (on the ARGB header)", &pumpRgb)) {
            pc::Layout c = l;
            c.pumpRgb = pumpRgb;
            SaveLayout(ctl, c);
        }
        if (l.pumpRgb) Muted("It shows the fans' lighting, as it's chained with them.");
    }
    int intake = 0, exhaust = 0;
    for (int i = 0; i < pc::kSlots; ++i)
        if (l.slots[static_cast<size_t>(i)] != pc::SlotFan::None) (l.Exhaust(i) ? exhaust : intake)++;
    if (l.Fans())
        Muted("%s", intake > exhaust   ? "More air in than out: positive pressure, less dust."
                    : intake < exhaust ? "More air out than in: negative pressure, dust comes in through the gaps."
                                       : "As much air in as out: balanced.");
    const int argb = ctl.config().argbFans.Fans();
    if (m.fansRgb && l.RgbFans() != argb)
        Muted("The ARGB header is set up for %d fan%s (Devices > Fans); %d are marked RGB here.", argb, argb == 1 ? "" : "s", l.RgbFans());
    if (!l.guessed) {
        ImGui::Dummy(ImVec2(0, 2 * S()));
        if (Btn("Back to LumaBridge's guess")) {
            ctl.prefs().caseLayout.clear();
            ctl.Changed();
        }
    }
    EndCard();

    ImGui::TableNextColumn();
    // What's in it, and which parts LumaBridge can light.
    BeginCard("found3d");
    CardTitle(f, "What LumaBridge found", Icon::Tower);
    auto row = [&](Icon icon, const std::string& name, const std::string& detail, bool rgb) {
        IconItem(icon, 16 * S(), Hex(rgb ? kAccent : kMuted));
        ImGui::SameLine();
        ImGui::TextUnformatted(name.c_str());
        if (!detail.empty()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));
            ImGui::TextUnformatted(detail.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(rgb ? "RGB" : "No RGB").x - 20 * S());
        RgbPill(rgb);
    };
    // Grouped like the PC: the core parts, graphics, cooling, storage, then the desk.
    bool firstGroup = true;
    auto group = [&](const char* name) {
        ImGui::Dummy(ImVec2(0, (firstGroup ? 2.f : 8.f) * S()));
        firstGroup = false;
        ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));
        ImGui::TextUnformatted(name);
        ImGui::PopStyleColor();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 1 * S()), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 1 * S()),
                                            Hex(kBorder), 1 * S());
        ImGui::Dummy(ImVec2(0, 2 * S()));
    };
    int sticks = 0;
    for (bool st : m.ram) sticks += st;
    group("Processor and motherboard");
    row(Icon::Cpu, m.cpuName.empty() ? "Processor" : m.cpuName, "", false);
    row(Icon::Board, m.boardName.empty() ? "Motherboard" : m.boardName, "", m.boardRgb);
    row(Icon::Memory, m.ramName.empty() ? "Memory" : m.ramName, std::to_string(sticks) + (sticks == 1 ? " stick" : " sticks"), m.ramRgb);
    if (!m.gpuName.empty()) {
        group("Graphics");
        row(Icon::Gpu, m.gpuName, "", m.gpuRgb);
    }
    group("Cooling");
    if (l.cooler == pc::Cooler::Aio) {
        std::string detail = "AIO, radiator " + std::string(l.radiator == pc::Mount::Top ? "on top" : "in front");
        if (m.kraken.valid) {
            char b[64];
            snprintf(b, sizeof b, ", liquid %.1f \xC2\xB0" "C, pump %d RPM", m.kraken.liquidC, m.kraken.pumpRpm);
            detail += b;
        }
        row(Icon::Fan, m.aio ? m.aio->name : "AIO water cooler", detail, l.pumpRgb && m.fansRgb);
    } else {
        row(Icon::Fan, "CPU cooler", m.cpuFanRpm > 0 ? "air, " + std::to_string(static_cast<int>(m.cpuFanRpm)) + " RPM" : "air", false);
    }
    row(Icon::Fan, "Case fans", std::to_string(l.Fans()) + ", " + std::to_string(l.RgbFans()) + " RGB", m.fansRgb && l.RgbFans() > 0);
    if (m.drives) {
        group("Storage");
        row(Icon::Disk, "Drives", std::to_string(m.drives) + (m.drives == 1 ? " drive" : " drives"), false);
    }
    if (!m.screens.empty()) {
        group("Monitors");
        Muted("If a size is wrong, enter the inches from the monitor's label.");
        for (const auto& d : m.screens) {
            float w = 0, h = 0;
            displays::ScreenSize(d, &w, &h);
            char b[96];
            snprintf(b, sizeof b, "%.1f\", %dx%d%s", std::sqrt(w * w + h * h) / 2.54f, d.w, d.h, d.primary ? ", main" : "");
            std::string detail = b;
            if (d.hz > 0) detail += ", " + std::to_string(d.hz) + " Hz";
            row(Icon::Grid, d.name, detail, false);
            const std::string key = displays::SizeKey(d);
            ImGui::PushID(key.c_str());
            float inches = std::sqrt(w * w + h * h) / 2.54f;
            ImGui::SetNextItemWidth(130 * S());
            if (ImGui::InputFloat("Size (inches)", &inches, 0.1f, 1.f, "%.1f") && displays::ValidDiagonal(inches)) {
                ctl.prefs().monitorSizes[key] = inches;
                ctl.Changed();
            }
            ImGui::BeginDisabled(!ctl.prefs().monitorSizes.count(key));
            if (SmallBtn("Use reported size")) {
                ctl.prefs().monitorSizes.erase(key);
                ctl.Changed();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }
    group("On the desk");
    row(Icon::Keyboard, m.keyboard.name, "", m.keyboard.device != nullptr);
    row(Icon::Mouse, m.mouse.name, "", m.mouse.device != nullptr);
    if (m.headset) row(Icon::Leds, m.headsetGear.name, "", m.headsetGear.device != nullptr);
    if (m.controller) row(Icon::Game, m.controllerGear.name, "", m.controllerGear.device != nullptr);
    for (const auto& g : m.others) row(Icon::Leds, g.name, "", true);
    EndCard();
    ImGui::EndTable();
}

// Hands a device back to its own app, or takes it again. The fans and the motherboard share
// one ASUS controller, so they go together.
void SetNative(Controller& ctl, const std::string& id, bool native) {
    auto& all = ctl.prefs().deviceLighting;
    all[id].native = native;
    if (id == device::kFans || id == device::kBoard) {
        all[device::kFans].native = native;
        all[device::kBoard].native = native;
    }
}

void NativeNote(const std::string& id) {
    if (id == device::kFans || id == device::kBoard)
        Muted("Armoury Crate lights the fans and the motherboard (they share one ASUS controller, so both go "
              "back together). LumaBridge leaves them alone, games included.");
    else if (id == device::kOther)
        Muted("These devices show their own lighting again (their firmware's, or what OpenRGB had). "
              "LumaBridge leaves them alone, games included.");
    else
        Muted("%s lights it again. LumaBridge leaves it alone, games included.", NativeApp(id));
}

// Lighting > Manual: the selected color and setup side by side, with editing controls below.
void ManualPage(Controller& ctl, UiState& ui, const Fonts& f) {
    Prefs& p = ctl.prefs();
    // Which lighting to edit: everything, or one device.
    std::vector<std::string> ids{""};
    std::vector<const char*> labels{"All devices"};
    for (const char* id : LitDeviceIds(ctl)) {
        ids.push_back(id);
        labels.push_back(device::Name(id));
    }
    int sel = 0;
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == ui.lightTarget) sel = static_cast<int>(i);
    ui.lightTarget = ids[static_cast<size_t>(sel)];  // a device that was turned off: back to all
    ManualOverview(ctl, ui, f);
    // A click in the preview can select another device in this frame.
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == ui.lightTarget) sel = static_cast<int>(i);
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted("Lighting to edit");
    ImGui::PopFont();
    float targetWidth = 0;
    for (const char* label : labels) targetWidth = std::max(targetWidth, ImGui::CalcTextSize(label).x + 28 * S());
    if (targetWidth * labels.size() > ImGui::GetContentRegionAvail().x) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##target", &sel, labels.data(), static_cast<int>(labels.size())))
            ui.lightTarget = ids[static_cast<size_t>(sel)];
    } else if (Segmented("target", &sel, labels.data(), static_cast<int>(labels.size()), ImGui::GetContentRegionAvail().x))
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
        int mode = d.native ? 2 : d.own ? 1 : 0;
        const std::string back = std::string("Back to ") + NativeApp(ui.lightTarget);
        const char* modes[] = {"Same as all devices", "Its own lighting", back.c_str()};
        bool modeChanged;
        if (ImGui::GetContentRegionAvail().x < 640 * S()) {
            ImGui::SetNextItemWidth(-1);
            modeChanged = ImGui::Combo("##own", &mode, modes, 3);
        } else {
            modeChanged = Segmented("own", &mode, modes, 3, std::min(620 * S(), ImGui::GetContentRegionAvail().x));
        }
        if (modeChanged) {
            if (mode == 1 && !d.own && d.look == Look{}) d.look = MainLook(p);  // start from what it shows now
            if (mode != 2) d.own = mode == 1;
            SetNative(ctl, ui.lightTarget, mode == 2);
            ctl.Changed();
        }
        if (d.native)
            NativeNote(ui.lightTarget);
        else if (ui.lightTarget == device::kKeyboard)
            Muted("The Azoth shows the effect key by key, by cable or through its Omni receiver.");
        else if (ui.lightTarget == device::kOther)
            Muted("Devices with Windows' lighting standard (and OpenRGB's, if you use it) show this lighting, "
                  "across each device from left to right.");
        else if (ui.lightTarget == device::kMouse)
            Muted("%s", ctl.logitech().mouseEffect() ? "It shows the effect LED by LED."
                                                      : "Through G HUB it shows one color: the effect's first LED.");
        else if (ui.lightTarget == device::kController)
            Muted("The lightbar shows the effect's first color, by USB or Bluetooth.");
        else if (!d.own)
            Muted("It shows the same lighting as the rest. Pick \"Its own lighting\" to set it apart.");
        EndCard();
        if (d.own && !d.native && LookEditor(ctl, ui, f, d.look)) ctl.Changed();
    }
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
    if (!ctl.prefs().openRgb) return {"Detected, control off", kMuted, "Enable OpenRGB lighting in Integrations."};
    if (!ctl.OpenRgbOn(d))
        return ctl.OpenRgbDefaultOn(d) ? DeviceStatus{"Off", kMuted}
                                       : DeviceStatus{"Lit by LumaBridge itself", kMuted,
                                                      "LumaBridge lights this device directly, so it's left out here."};
    return ctl.output().stopped ? DeviceStatus{"Its own effect", kMuted} : DeviceStatus{"Following LumaBridge", kGreen};
}

DeviceStatus LampArrayStatus(const Controller& ctl, const LampArrayDevice& d) {
    if (!d.problem.empty()) return {"Can't light it", kAmber, d.problem};
    if (!ctl.prefs().lampArray) return {"Detected, control off", kMuted};
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

DeviceStatus DualSenseStatus(Controller& ctl) {
    const auto& ds = ctl.dualsense();
    using D_ = DualSenseOutput::State;
    if (!ctl.prefs().dualsenseController) return {"Off", kMuted};
    switch (ds.state()) {
    case D_::Active: return {ds.bluetooth() ? "Following LumaBridge (Bluetooth)" : "Following LumaBridge (USB)", kGreen};
    case D_::Connecting: return {"Connecting (Bluetooth)", kAmber};
    case D_::NotFound: return {"Not connected", kAmber};
    default: return {"The game or Steam's lighting", kMuted};
    }
}

// "Fade out when not used": the switch and the delay (seconds, shown in minutes from 1 minute).
bool SleepControls(const char* id, bool* on, int* seconds, bool* ignoreDynamic) {
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
        ImGui::Dummy(ImVec2(0, 4 * S()));
        if (Toggle("Refuse to sleep while a dynamic effect is showing", ignoreDynamic)) changed = true;
        Muted("Games and animated presets (Rainbow, Comet, ...) keep it awake instead of fading out mid-effect; "
              "a still color still fades and sleeps as usual.");
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
    if (SleepControls("logisleep", &ctl.prefs().logitechSleep, &ctl.prefs().logitechSleepSec,
                      &ctl.prefs().logitechSleepIgnoreDynamic))
        ctl.Changed();
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
          "or through its ROG Omni receiver (a little slower there, to spare the battery). LumaBridge talks to the "
          "keyboard directly (no Armoury Crate needed), and never sends Armoury Crate's save command, so your saved "
          "Armoury Crate lighting stays in the keyboard. When LumaBridge lets go, the keyboard keeps the last colors "
          "until it restarts or Armoury Crate sets it again.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    bool enabled = ctl.prefs().azothKeyboard;
    if (Toggle("Light the ROG Azoth", &enabled)) ctl.SetAzothEnabled(enabled);
    if (enabled) {
        ImGui::SameLine();
        using A_ = AzothOutput::State;
        const A_ st = ctl.azoth().state();
        if (st == A_::Active) Pill(ctl.azoth().wireless() ? "Active (wireless)" : "Active (wired)", kGreen);
        else if (st == A_::Released) Pill("Handed off", kMuted);
        else Pill("Not found", kAmber);
        if (st == A_::NotFound) {
            const unsigned long err = ctl.azoth().lastWriteError();
            if (err)
                Muted("It was connected, then a write failed (error %lu) - unplugged, asleep, or a cable/receiver "
                      "issue. LumaBridge keeps trying. Details are in the log (Settings).",
                      err);
            else
                Muted("Not found yet, by cable or its Omni receiver; LumaBridge keeps trying every few seconds. "
                      "It doesn't need Armoury Crate or any ASUS software running - if it's plugged in and still "
                      "not showing up, run the device probe below to see what LumaBridge actually sees.");
        }
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (SleepControls("azothsleep", &ctl.prefs().azothSleep, &ctl.prefs().azothSleepSec,
                      &ctl.prefs().azothSleepIgnoreDynamic))
        ctl.Changed();
    Muted("When you haven't typed for a while, the keys fade out and LumaBridge stops sending, so the keyboard can "
          "sleep (and save its battery wirelessly). The next key press lights it up again.");
    ImGui::SameLine();
    if (SmallBtn("Run the device probe")) {
        const std::wstring exe = AppDirectory() + L"\\tools\\device-probe.exe";
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    EndCard();
}

// ---- Game controller: live view, battery, report timing and button mapping -----------------

// The controller in 3D, live: held buttons light up and sink, the sticks lean, the triggers
// travel and fingers show on the touchpad, with the lightbar in its current color. Drag to
// turn it, scroll to zoom, double-click to put it back.
void PadModelView(const pad::State& st, Rgb lightbar, bool lit, float height) {
    static s3d::Camera cam;
    static bool camSet = false;
    auto reset = [] {
        cam = {};
        cam.target = {0, 2.f, 0.2f};
        cam.yaw = 0.3f;  // a little from the side, so the triggers show
        cam.pitch = 0.95f;
        cam.distance = 23.f;
    };
    if (!camSet) {
        reset();
        camSet = true;
    }
    const float W = ImGui::GetContentRegionAvail().x;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + W, a.y + height), Hex(0x141A26), Hex(0x141A26), Hex(0x07090D), Hex(0x07090D));
    ImGui::InvisibleButton("pad-model", ImVec2(W, height));
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered()) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        if (io.MouseWheel != 0) cam.distance = std::clamp(cam.distance * std::pow(0.9f, io.MouseWheel), 14.f, 60.f);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) reset();
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
        cam.yaw -= io.MouseDelta.x * 0.01f;
        cam.pitch = std::clamp(cam.pitch + io.MouseDelta.y * 0.01f, 0.1f, 1.5f);
    }

    s3d::Scene sc;
    controller3d::Build(sc, 0, view3d::C(lightbar), lit, &st, s3d::Rgba(0x7C, 0x6C, 0xFF));
    const s3d::Viewport vp{a.x, a.y, W, height};
    const auto items = s3d::Render(sc, cam, vp);
    dl->PushClipRect(a, ImVec2(a.x + W, a.y + height), true);
    const uintptr_t image = scene_gpu::Image(3, items, vp, S());
    if (image) dl->AddImage(static_cast<ImTextureID>(image), a, ImVec2(a.x + W, a.y + height));
    else PaintItems(dl, items);
    const char* hint = "Drag to turn, scroll to zoom, double-click to reset";
    dl->AddText(ImVec2(a.x + 12 * S(), a.y + height - ImGui::GetTextLineHeight() - 10 * S()), Hex(kMuted, 170), hint);
    dl->PopClipRect();
}

// The controller's state a few times a second, for numbers: at the screen's rate they change
// too quickly to read. The drawings use the live state.
const pad::State& SlowState(const pad::State& now) {
    static pad::State held;
    static double heldAt = -1;
    const double t = ImGui::GetTime();
    if (t - heldAt > 0.2 || heldAt < 0) {
        held = now;
        heldAt = t;
    }
    return held;
}

// A box with a small title, for one group of inputs under the 2D controller.
void InputPanel(ImDrawList* dl, ImVec2 a, ImVec2 size, const char* title) {
    dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), Hex(0x10141B), 10 * S());
    dl->AddRect(a, ImVec2(a.x + size.x, a.y + size.y), Hex(kBorder, 200), 10 * S());
    dl->AddText(ImVec2(a.x + 12 * S(), a.y + 9 * S()), Hex(kMuted), title);
}

// Text centered on `x`.
void CenteredText(ImDrawList* dl, float x, float y, ImU32 col, const char* text) {
    dl->AddText(ImVec2(x - ImGui::CalcTextSize(text).x / 2, y), col, text);
}

// A trigger as a vertical bar that fills from the bottom as it's squeezed.
void TriggerBar(ImDrawList* dl, ImVec2 a, ImVec2 size, uint8_t value, const char* label, int number) {
    const ImVec2 b(a.x + size.x, a.y + size.y);
    const float r = size.x / 2;
    CenteredText(dl, a.x + size.x / 2, a.y - ImGui::GetTextLineHeight() - 6 * S(), Hex(value > 0 ? kText : kMuted), label);
    dl->AddRectFilled(a, b, Hex(kTrack), r);
    const float fill = size.y * value / 255.f;
    if (fill > 1) dl->AddRectFilled(ImVec2(a.x, b.y - fill), b, Hex(kAccent), r);
    dl->AddRect(a, b, Hex(kBorder), r);
    if (number >= 0) {
        char text[8];
        snprintf(text, sizeof text, "%d", number);
        CenteredText(dl, a.x + size.x / 2, b.y + 6 * S(), Hex(kText), text);
    }
}

// A stick: its round travel with crosshairs, and a dot where it is (clicked: a lit ring).
void StickPanel(ImDrawList* dl, ImVec2 a, ImVec2 size, const char* title, uint8_t x, uint8_t y, bool click,
                const pad::State* numbers, bool right) {
    InputPanel(dl, a, size, title);
    const float top = a.y + 30 * S(), bottom = a.y + size.y - (numbers ? 30 * S() : 12 * S());
    const float r = std::max(10.f, std::min(size.x - 24 * S(), bottom - top) / 2);
    const ImVec2 c(a.x + size.x / 2, (top + bottom) / 2);
    dl->AddCircleFilled(c, r, Hex(0x0A0D12), 40);
    dl->AddCircle(c, r, Hex(click ? kAccentHover : kBorder), 40, click ? 2.5f * S() : 1.f * S());
    dl->AddCircle(c, r * 0.5f, Hex(kBorder, 110), 32);
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), Hex(kBorder, 140));
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), Hex(kBorder, 140));
    auto axis = [](uint8_t v) { return std::clamp((static_cast<float>(v) - 128.f) / 127.f, -1.f, 1.f); };
    const ImVec2 d(c.x + axis(x) * (r - 6 * S()), c.y + axis(y) * (r - 6 * S()));
    dl->AddLine(c, d, Hex(kAccent, 140), 2 * S());
    dl->AddCircleFilled(d, 7 * S(), Hex(click ? kAccentHover : kAccent), 20);
    dl->AddCircle(d, 7 * S(), Hex(0xFFFFFF, 90), 20, 1.f * S());
    if (click) dl->AddText(ImVec2(a.x + size.x - 30 * S(), a.y + 9 * S()), Hex(kAccentHover), right ? "R3" : "L3");
    if (numbers) {
        char text[32];
        const int nx = right ? numbers->rx : numbers->lx, ny = right ? numbers->ry : numbers->ly;
        snprintf(text, sizeof text, "X %d   Y %d", nx - 128, ny - 128);
        CenteredText(dl, c.x, a.y + size.y - 24 * S(), Hex(kText), text);
    }
}

// The touchpad, to scale, with each finger where it touches (and lit edges while clicked).
void TouchPanel(ImDrawList* dl, ImVec2 a, ImVec2 size, const pad::State& st, const pad::State* numbers) {
    InputPanel(dl, a, size, "Touchpad");
    const float top = a.y + 30 * S(), bottom = a.y + size.y - (numbers ? 30 * S() : 12 * S());
    float w = size.x - 24 * S(), h = w * 1080.f / 1920.f;
    if (h > bottom - top) {
        h = bottom - top;
        w = h * 1920.f / 1080.f;
    }
    const ImVec2 p0(a.x + (size.x - w) / 2, (top + bottom - h) / 2), p1(p0.x + w, p0.y + h);
    const bool clicked = st.Down(pad::kTouchpad);
    dl->AddRectFilled(p0, p1, Hex(clicked ? 0x2C2860 : 0x262B35), 6 * S());
    dl->AddRect(p0, p1, Hex(clicked ? kAccentHover : kBorder), 6 * S(), 0, clicked ? 2.f * S() : 1.f * S());
    for (int i = 0; i < 2; ++i) {
        const pad::Touch& t = st.touch[i];
        if (!t.down) continue;
        const ImVec2 at(p0.x + w * std::clamp(t.x / 1919.f, 0.f, 1.f), p0.y + h * std::clamp(t.y / 1079.f, 0.f, 1.f));
        const unsigned col = i ? kAccent2 : kAccent;
        dl->AddCircleFilled(at, 11 * S(), Hex(col, 60), 24);
        dl->AddCircleFilled(at, 6 * S(), Hex(col), 20);
        CenteredText(dl, at.x, at.y - 26 * S(), Hex(col), i ? "2" : "1");
    }
    if (numbers) {
        std::string text;
        for (int i = 0; i < 2; ++i) {
            if (!numbers->touch[i].down) continue;
            if (!text.empty()) text += "    ";
            text += std::to_string(i + 1) + ": " + std::to_string(numbers->touch[i].x) + ", " + std::to_string(numbers->touch[i].y);
        }
        CenteredText(dl, a.x + size.x / 2, a.y + size.y - 24 * S(), Hex(text.empty() ? kMuted : kText),
                     text.empty() ? "No finger" : text.c_str());
    }
}

// Gyroscope and accelerometer: one bar per axis, growing either way from the middle.
void MotionPanel(ImDrawList* dl, ImVec2 a, ImVec2 size, const pad::State& st, const pad::State* numbers) {
    InputPanel(dl, a, size, "Motion");
    static const char* names[6] = {"Gyro X", "Gyro Y", "Gyro Z", "Accel X", "Accel Y", "Accel Z"};
    const float top = a.y + 32 * S(), rowH = (size.y - 32 * S() - 10 * S()) / 6;
    const float labelW = 64 * S(), numberW = numbers ? 56 * S() : 0;
    const float x0 = a.x + 12 * S() + labelW, x1 = a.x + size.x - 12 * S() - numberW;
    for (int i = 0; i < 6; ++i) {
        const float y = top + rowH * i, mid = y + rowH / 2;
        const int value = i < 3 ? st.gyro[i] : st.accel[i - 3];
        const float range = i < 3 ? 8000.f : 10000.f;  // raw units; about 1.2 g for the accelerometer
        dl->AddText(ImVec2(a.x + 12 * S(), mid - ImGui::GetTextLineHeight() / 2), Hex(kMuted), names[i]);
        const float bh = std::min(8 * S(), rowH * 0.5f), cx = (x0 + x1) / 2;
        dl->AddRectFilled(ImVec2(x0, mid - bh / 2), ImVec2(x1, mid + bh / 2), Hex(kTrack), bh / 2);
        const float fill = std::clamp(value / range, -1.f, 1.f) * (x1 - x0) / 2;
        if (std::fabs(fill) > 0.5f)
            dl->AddRectFilled(ImVec2(std::min(cx, cx + fill), mid - bh / 2), ImVec2(std::max(cx, cx + fill), mid + bh / 2),
                              Hex(i < 3 ? kAccent : kAccent2), bh / 2);
        dl->AddLine(ImVec2(cx, mid - bh), ImVec2(cx, mid + bh), Hex(kMuted, 120));
        if (numbers) {
            char text[16];
            snprintf(text, sizeof text, "%d", i < 3 ? numbers->gyro[i] : numbers->accel[i - 3]);
            dl->AddText(ImVec2(a.x + size.x - 12 * S() - ImGui::CalcTextSize(text).x, mid - ImGui::GetTextLineHeight() / 2),
                        Hex(kText), text);
        }
    }
}

// Every button as a chip, lit while held.
void ButtonChips(const pad::State& st) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float W = ImGui::GetContentRegionAvail().x, gap = 6 * S(), h = ImGui::GetTextLineHeight() + 8 * S();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float x = 0, y = 0;
    for (int b = 0; b < pad::kButtonCount; ++b) {
        if (b == pad::kL2 || b == pad::kR2) continue;  // the trigger bars show them
        if (b == pad::kMute && st.model == pad::Model::DualShock4) continue;
        const char* name = pad::ButtonName(st.model, b);
        const float w = ImGui::CalcTextSize(name).x + 20 * S();
        if (x > 0 && x + w > W) {
            x = 0;
            y += h + gap;
        }
        const bool on = st.Down(b);
        const ImVec2 a(o.x + x, o.y + y), c(a.x + w, a.y + h);
        dl->AddRectFilled(a, c, Hex(on ? kAccent : 0x10141B), h / 2);
        dl->AddRect(a, c, Hex(on ? kAccentHover : kBorder), h / 2);
        dl->AddText(ImVec2(a.x + 10 * S(), a.y + 4 * S()), Hex(on ? 0xFFFFFF : kMuted), name);
        x += w + gap;
    }
    ImGui::Dummy(ImVec2(W, y + h));
}

// Every input at once: the controller from above with what's held lit, the triggers either
// side, then the sticks, touchpad and motion in their own boxes, and every button by name.
// `numbers`: also the raw values (a few times a second).
void PadView2d(const pad::State& st, Rgb lightbar, bool lit, bool numbers) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float W = ImGui::GetContentRegionAvail().x;
    const pad::State* slow = numbers ? &SlowState(st) : nullptr;

    // The controller with its triggers either side.
    const float cw = std::min(W - 170 * S(), 540 * S()), ch = cw * 84.f / 132.f;
    const float stageH = ch + 2 * 34 * S();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    dl->AddRectFilledMultiColor(o, ImVec2(o.x + W, o.y + stageH), Hex(0x141A26), Hex(0x141A26), Hex(0x0B0E14), Hex(0x0B0E14));
    const ImVec2 ca(o.x + (W - cw) / 2, o.y + 34 * S());
    DrawController(dl, ca, ImVec2(cw, ch), lightbar, lit, &st);
    const ImVec2 bar(18 * S(), ch * 0.72f);
    const float barY = ca.y + ch * 0.1f;
    TriggerBar(dl, ImVec2(ca.x - 54 * S() - bar.x, barY), bar, st.l2, "L2", slow ? slow->l2 : -1);
    TriggerBar(dl, ImVec2(ca.x + cw + 54 * S(), barY), bar, st.r2, "R2", slow ? slow->r2 : -1);
    ImGui::Dummy(ImVec2(W, stageH));
    ImGui::Dummy(ImVec2(0, 4 * S()));

    // Sticks, touchpad and motion: four across, or two by two when narrow.
    const int cols = W >= 760 * S() ? 4 : 2, rows = 4 / cols;
    const float gap = 10 * S(), pw = (W - gap * (cols - 1)) / cols, ph = 190 * S();
    const ImVec2 g = ImGui::GetCursorScreenPos();
    auto cell = [&](int i) { return ImVec2(g.x + (i % cols) * (pw + gap), g.y + (i / cols) * (ph + gap)); };
    StickPanel(dl, cell(0), ImVec2(pw, ph), "Left stick", st.lx, st.ly, st.Down(pad::kL3), slow, false);
    StickPanel(dl, cell(1), ImVec2(pw, ph), "Right stick", st.rx, st.ry, st.Down(pad::kR3), slow, true);
    TouchPanel(dl, cell(2), ImVec2(pw, ph), st, slow);
    MotionPanel(dl, cell(3), ImVec2(pw, ph), st, slow);
    ImGui::Dummy(ImVec2(W, rows * ph + (rows - 1) * gap));
    ImGui::Dummy(ImVec2(0, 6 * S()));
    ButtonChips(st);
}

// The lightbar's color: a lit swatch, its hex and RGB values (refreshed a few times a second so
// a moving effect stays readable), and whether LumaBridge is lighting it.
void LightbarRow(Controller& ctl, Rgb c, bool lit) {
    static Rgb shown{};
    static double shownAt = -1;
    const double now = ImGui::GetTime();
    if (now - shownAt > 0.25 || shownAt < 0) {
        shown = c;
        shownAt = now;
    }
    const float sw = 30 * S();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cc(p.x + sw / 2, p.y + sw / 2);
    if (lit)
        for (int i = 3; i >= 1; --i) dl->AddCircleFilled(cc, sw / 2 + i * 3 * S(), IM_COL32(c.r, c.g, c.b, 18 + (3 - i) * 14), 32);
    dl->AddCircleFilled(cc, sw / 2, IM_COL32(c.r, c.g, c.b, 255), 32);
    dl->AddCircle(cc, sw / 2, IM_COL32(255, 255, 255, 50), 32, 1.f * S());
    ImGui::Dummy(ImVec2(sw + 12 * S(), sw));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Lightbar");
    ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));
    if (lit) ImGui::Text("#%02X%02X%02X   Red %d, green %d, blue %d", shown.r, shown.g, shown.b, shown.r, shown.g, shown.b);
    else if (!ctl.prefs().dualsenseController) ImGui::TextUnformatted("Lightbar control is off (Settings, below)");
    else ImGui::TextUnformatted("Not lit by LumaBridge right now");
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

// Raw readings, a few times a second: they change too quickly to read at the screen's rate.
void PadReadings(const pad::State& now) {
    static pad::State st;
    static double shownAt = -1;
    const double t = ImGui::GetTime();
    if (t - shownAt > 0.2 || shownAt < 0) {
        st = now;
        shownAt = t;
    }
    auto stick = [](uint8_t v) { return static_cast<int>(v) - 128; };
    if (ImGui::BeginTable("pad-readings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthFixed, 130 * S());
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        auto row = [](const char* what, const char* fmt, auto... args) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Muted("%s", what);
            ImGui::TableNextColumn();
            ImGui::Text(fmt, args...);
        };
        row("Left stick", "%d, %d", stick(st.lx), stick(st.ly));
        row("Right stick", "%d, %d", stick(st.rx), stick(st.ry));
        row("Triggers", "L2 %d   R2 %d  (of 255)", st.l2, st.r2);
        row("Touches", "%d", (st.touch[0].down ? 1 : 0) + (st.touch[1].down ? 1 : 0));
        row("Gyroscope", "%d, %d, %d", st.gyro[0], st.gyro[1], st.gyro[2]);
        row("Accelerometer", "%d, %d, %d", st.accel[0], st.accel[1], st.accel[2]);
        ImGui::EndTable();
    }
}

void PadLiveCard(Controller& ctl, const Fonts& f, const PadInput::Snapshot& snap) {
    BeginCard("pad-live");
    CardTitle(f, "Live", Icon::Gauge);
    bool input = ctl.prefs().padInput;
    if (Toggle("Read the controller", &input)) ctl.SetPadInputEnabled(input);
    if (!input) {
        Muted("Switched off. Nothing is read from the controller, and no mapping runs.");
        EndCard();
        return;
    }
    if (!snap.connected) {
        Muted("No DualSense or DualShock 4 found by USB or Bluetooth yet; LumaBridge keeps looking.");
        if (snap.error) Muted("The last read failed (error %lu): unplugged, or out of Bluetooth range?", snap.error);
        EndCard();
        return;
    }
    const pad::State& st = snap.state;
    Muted("%s by %s. Press, push and squeeze: the view follows.%s", pad::ModelName(st.model),
          snap.bluetooth ? "Bluetooth" : "USB cable", st.model == pad::Model::DualShock4 ? " (Drawn as a DualSense.)" : "");
    if (snap.bluetooth && snap.reports > 60 && st.battery < 0)
        Muted("Only the short Bluetooth report is arriving, so there is no battery or motion data. Reconnect the controller.");
    ImGui::Dummy(ImVec2(0, 4 * S()));

    const Rgb color = view3d::Leds(ctl, device::kController, ImGui::GetTime(), 1)[0];
    const bool lit = ctl.prefs().dualsenseController && ctl.dualsense().state() == DualSenseOutput::State::Active &&
                     LiveParams(ctl, device::kController) != nullptr;
    // The numbers switch, with the 2D / 3D choice across from it. 2D shows every input at
    // once; 3D is the model alone.
    const float lineEnd = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    bool values = ctl.prefs().padShowValues;
    if (Toggle("Show numbers", &values)) {
        ctl.prefs().padShowValues = values;
        ctl.Changed();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stick, trigger, touch and motion values, a few times a second.\n"
                          "Off by default: they change many times a second.");
    ImGui::SameLine(lineEnd - 140 * S());
    int view = ctl.prefs().padView3d ? 1 : 0;
    const char* views[] = {"2D", "3D"};
    if (Segmented("pad-view", &view, views, 2, 140 * S())) {
        ctl.prefs().padView3d = view == 1;
        ctl.Changed();
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (view == 1) {
        PadModelView(st, color, lit, 320 * S());
        if (values) {
            ImGui::Dummy(ImVec2(0, 6 * S()));
            PadReadings(st);
        }
    } else {
        PadView2d(st, color, lit, values);
    }
    ImGui::Dummy(ImVec2(0, 8 * S()));
    LightbarRow(ctl, lit ? color : Rgb{50, 54, 64}, lit);
    EndCard();
}

void PadBatteryCard(const Fonts& f, const PadInput::Snapshot& snap) {
    BeginCard("pad-battery");
    CardTitle(f, "Battery", Icon::Bolt);
    if (!snap.connected) {
        Muted("Connect the controller to see its battery.");
        EndCard();
        return;
    }
    const pad::State& st = snap.state;
    if (st.battery < 0) {
        Muted("The controller isn't reporting its battery over this connection yet.");
        EndCard();
        return;
    }
    const unsigned color = st.charging || st.full ? kGreen : st.battery <= 15 ? kRed : st.battery <= 30 ? kAmber : kAccent;
    ImGui::PushFont(f.title);
    ImGui::Text("%d%%", st.battery);
    ImGui::PopFont();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float w = std::min(ImGui::GetContentRegionAvail().x, 320 * S()), h = 12 * S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), Hex(kTrack), h / 2);
    dl->AddRectFilled(a, ImVec2(a.x + w * st.battery / 100.f, a.y + h), Hex(color), h / 2);
    ImGui::Dummy(ImVec2(w, h + 4 * S()));
    Pill(st.full ? "Fully charged" : st.charging ? "Charging" : snap.bluetooth ? "On battery" : "On the cable", color);
    Muted("The controller reports its charge in steps of 10 %%, so this moves in steps too.");
    EndCard();
}

void PadTimingCard(Controller& ctl, const Fonts& f, const PadInput::Snapshot& snap) {
    BeginCard("pad-timing");
    CardTitle(f, "Report timing", Icon::Gauge);
    Muted("How often the controller sends its state, and how even that is. Lower and steadier is better. Bluetooth is "
          "slower than a cable. This is the controller's own timing, not the delay inside a game.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    bool show = ctl.prefs().padShowTiming;
    if (Toggle("Show live timing", &show)) {
        ctl.prefs().padShowTiming = show;
        ctl.Changed();
    }
    if (!show) {
        Muted("Off by default: the numbers and graph change many times a second.");
        EndCard();
        return;
    }
    if (!snap.connected) {
        Muted("Connect the controller to see how steadily it reports.");
        EndCard();
        return;
    }
    // The numbers twice a second, so they can be read; the graph stays live.
    static PadInput::Snapshot held;
    static double heldAt = -1;
    const double now = ImGui::GetTime();
    if (now - heldAt > 0.5 || heldAt < 0) {
        held = snap;
        heldAt = now;
    }
    auto ms = [](double v) {
        char text[24];
        snprintf(text, sizeof text, "%.1f ms", v);
        return std::string(text);
    };
    ImGui::Dummy(ImVec2(0, 2 * S()));
    PillFlow({{std::to_string(static_cast<int>(held.rateHz + 0.5)) + " reports a second", kAccent},
              {"Average " + ms(held.averageMs), kMuted},
              {"Slowest " + ms(held.maxMs), held.maxMs > 30 ? kAmber : kMuted},
              {"Jitter " + ms(held.jitterMs), held.jitterMs > 5 ? kAmber : kMuted}});
    ImGui::Dummy(ImVec2(0, 4 * S()));
    // Graph of the last intervals, scaled to at least 20 ms.
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 70 * S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), Hex(kBg), 8 * S());
    const auto& hist = snap.history;
    float top = 20.f;
    for (float v : hist) top = std::max(top, v);
    if (hist.size() > 1) {
        std::vector<ImVec2> pts;
        for (size_t i = 0; i < hist.size(); ++i)
            pts.push_back(ImVec2(a.x + w * i / (hist.size() - 1), a.y + h - 4 * S() - (h - 8 * S()) * std::min(hist[i], top) / top));
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), Hex(kAccent), 0, 1.6f * S());
    }
    char scale[24];
    snprintf(scale, sizeof scale, "%.0f ms", top);
    dl->AddText(ImVec2(a.x + 6 * S(), a.y + 4 * S()), Hex(kMuted), scale);
    ImGui::Dummy(ImVec2(w, h));
    EndCard();
}

bool BindingCombo(const char* id, pad::Binding* b) {
    bool changed = false;
    ImGui::SetNextItemWidth(190 * S());
    if (ImGui::BeginCombo(id, pad::BindingLabel(*b).c_str())) {
        if (ImGui::Selectable("Not mapped", b->action == pad::Action::None)) { *b = pad::Binding{}; changed = true; }
        ImGui::SeparatorText("Mouse");
        for (uint16_t c = 1; c <= 5; ++c)
            if (ImGui::Selectable(pad::MouseName(c), b->action == pad::Action::Mouse && b->code == c)) {
                *b = {pad::Action::Mouse, c};
                changed = true;
            }
        ImGui::SeparatorText("Keyboard");
        for (const auto& k : pad::Keys())
            if (ImGui::Selectable(k.name, b->action == pad::Action::Key && b->code == k.vk)) {
                *b = {pad::Action::Key, k.vk};
                changed = true;
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return changed;
}

void PadMappingCard(Controller& ctl, const Fonts& f, const PadInput::Snapshot& snap) {
    pad::Mapping& m = ctl.prefs().padMapping;
    BeginCard("pad-mapping");
    CardTitle(f, "Button mapping", Icon::Keyboard);
    Muted("Make a controller button press a keyboard key or a mouse button, and optionally move the mouse with the right "
          "stick. This adds keyboard and mouse input only: games still see the controller itself, and there is no "
          "virtual gamepad (that needs a driver). Some online games with anti-cheat may not accept it.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    bool changed = false;
    changed |= Toggle("Use this mapping", &m.enabled);
    if (m.enabled) {
        ImGui::SameLine();
        if (!ctl.prefs().padInput) Pill("Controller reading is off", kAmber);
        else if (!snap.connected) Pill("Waiting for the controller", kAmber);
        else if (!m.AnyMapped()) Pill("Nothing mapped yet", kMuted);
        else Pill("Active", kGreen);
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    changed |= Toggle("Right stick moves the mouse", &m.rightStickMouse);
    if (m.rightStickMouse) {
        float speed = static_cast<float>(m.mouseSpeed), dz = static_cast<float>(m.deadzone);
        if (LabeledSlider("Mouse speed", &speed, 1, 30, "%.0f")) { m.mouseSpeed = static_cast<int>(speed); changed = true; }
        if (LabeledSlider("Dead zone", &dz, 0, 50, "%.0f%%")) { m.deadzone = static_cast<int>(dz); changed = true; }
    }
    ImGui::Dummy(ImVec2(0, 6 * S()));
    const pad::Model model = snap.connected ? snap.state.model : pad::Model::DualSense;
    if (ImGui::BeginTable("pad-map", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("Does", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        for (int b = 0; b < pad::kButtonCount; ++b) {
            if (b == pad::kMute && model == pad::Model::DualShock4) continue;  // not on a DualShock 4
            ImGui::PushID(b);
            ImGui::TableNextRow(0, ImGui::GetFrameHeight() + 8 * S());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const bool held = snap.connected && snap.state.Down(b);
            ImGui::PushStyleColor(ImGuiCol_Text, V4(held ? kAccentHover : kText));
            ImGui::TextUnformatted(pad::ButtonName(model, b));
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            changed |= BindingCombo("##binding", &m.buttons[static_cast<size_t>(b)]);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (Btn("Clear all")) {
        m.buttons = {};
        changed = true;
    }
    if (changed) ctl.Changed();
    EndCard();
}

void DualSenseCard(Controller& ctl, const Fonts& f) {
    BeginCard("dualsense");
    CardTitle(f, "Settings", Icon::Gear);
    Muted("USB and Bluetooth lightbar control are confirmed working.");
    Muted("The lightbar follows LumaBridge's effect as one color, by USB cable or Bluetooth. LumaBridge talks to "
          "the controller directly (no Steam or extra software needed), and never saves anything to it, so it goes "
          "back to whatever the game or Steam set as soon as LumaBridge lets go.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    bool enabled = ctl.prefs().dualsenseController;
    if (Toggle("Light the DualSense lightbar", &enabled)) ctl.SetDualSenseEnabled(enabled);
    if (enabled) {
        ImGui::SameLine();
        using D_ = DualSenseOutput::State;
        const D_ st = ctl.dualsense().state();
        if (st == D_::Active) Pill(ctl.dualsense().bluetooth() ? "Active (Bluetooth)" : "Active (USB)", kGreen);
        else if (st == D_::Connecting) Pill("Connecting (Bluetooth)", kAmber);
        else if (st == D_::Released) Pill("Handed off", kMuted);
        else Pill("Not found", kAmber);
        if (st == D_::Connecting) Muted("Waiting a few seconds for the controller's startup lights.");
        if (st == D_::NotFound) {
            const unsigned long err = ctl.dualsense().lastWriteError();
            if (err)
                Muted("It was connected, then a write failed (error %lu) - unplugged, or out of Bluetooth range. "
                      "LumaBridge keeps trying. Details are in the log (Settings).",
                      err);
            else
                Muted("Not found yet, by USB or Bluetooth; LumaBridge keeps trying every few seconds.");
        }
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
        if (SmallBtn("Set up again")) in.Install("helper");
        ImGui::SameLine();
        if (SmallBtn("Remove")) in.Remove("helper");
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
    Muted("HyperX / Kingston FURY RGB DDR4 sticks show LumaBridge's effect across their five LEDs (supported AMD and Intel chipsets; Intel is experimental). "
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
    if (Btn("<  All devices")) {
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
    {
        bool native = DeviceNative(p, id);
        const std::string label = std::string("Let ") + NativeApp(id) + " light it";
        if (Toggle(label.c_str(), &native)) {
            SetNative(ctl, id, native);
            ctl.Changed();
        }
        if (native) {
            NativeNote(id);
            EndCard();
            return;
        }
    }
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
    Muted("Brightness: %.0f%% of the overall %.0f%%.%s", DeviceBrightness(p, id) * 100.0,
          ctl.config().auraCorrection.brightness * 100.0, DeviceReversed(p, id) ? " Direction: left." : " Direction: right.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (PrimaryButton("Change its lighting")) {
        ui.page = Page::Lighting;
        ui.lightTarget = id;
    }
    if (p.mode == Mode::Auto)
        Muted("Its own lighting and colors are set in Manual mode; games light every device alike.");
    EndCard();
}

// Firmware fills the gaps in Plug and Play: the motherboard and individual RAM modules
// are not necessarily Windows device nodes.
std::vector<inventory::Device> HardwareInventory(const Controller& ctl, const sensors::SystemSnapshot& snap) {
    auto devices = ctl.presence().inventory.devices;
    const auto& bios = snap.smbios;
    if (!bios.boardName.empty()) {
        inventory::Device d;
        d.id = "firmware:board";
        d.name = bios.boardName;
        d.className = "Motherboard";
        d.manufacturer = bios.boardMaker;
        d.category = inventory::Category::Components;
        devices.push_back(d);
    }
    for (size_t i = 0; i < bios.memory.size(); ++i) {
        const auto& m = bios.memory[i];
        inventory::Device d;
        d.id = "firmware:memory:" + std::to_string(i);
        d.name = m.part.empty() ? "Memory module" : m.part;
        d.className = "Memory module, " + Gb(static_cast<uint64_t>(m.sizeMb) * 1024 * 1024);
        if (m.speedMts) d.className += ", " + std::to_string(m.speedMts) + " MT/s";
        d.manufacturer = m.manufacturer;
        d.location = m.slot;
        d.category = inventory::Category::Components;
        devices.push_back(d);
    }
    inventory::Sort(devices);
    return devices;
}

DeviceStatus HardwareStatus(const inventory::Device& d) {
    if (d.problem) return {"Needs attention (code " + std::to_string(d.problem) + ")", kAmber};
    if (d.id.rfind("firmware:", 0) == 0) return {"From firmware", kMuted};
    return {"Connected", kGreen};
}

Icon HardwareIcon(inventory::Category c) {
    using C = inventory::Category;
    switch (c) {
    case C::Input: return Icon::Mouse;
    case C::Audio: return Icon::Gauge;
    case C::Displays: return Icon::Grid;
    case C::Graphics: return Icon::Gpu;
    case C::Components: return Icon::Cpu;
    case C::Storage: return Icon::Disk;
    case C::Network: case C::Bluetooth: case C::Usb: return Icon::Plug;
    case C::Cooling: return Icon::Fan;
    case C::System: return Icon::Gear;
    case C::Software: return Icon::Grid;
    default: return Icon::Info;
    }
}

void HardwareInventoryCard(Controller& ctl, UiState& ui, const Fonts& f, const sensors::SystemSnapshot& snap) {
    const auto devices = HardwareInventory(ctl, snap);
    int problems = 0;
    for (const auto& d : devices) problems += d.problem ? 1 : 0;
    BeginCard("inventory");
    CardTitle(f, "All hardware", Icon::Tower);
    Muted("Everything Windows reports as connected, plus your motherboard and memory from firmware. One device can "
          "appear several times, once per function. Lighting controls are on the Lighting and readings tab.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    const auto& scan = ctl.presence().inventory;
    if (scan.error) Muted("Windows could not finish the scan (error %lu). Rescan to try again.", scan.error);
    if (!ctl.presence().scanned) Muted("Scanning hardware...");
    PillFlow({{std::to_string(devices.size()) + (devices.size() == 1 ? " device" : " devices"), kAccent},
              {problems ? std::to_string(problems) + (problems == 1 ? " needs attention" : " need attention") : std::string("No problems"),
               problems ? kAmber : kGreen}});
    ImGui::Dummy(ImVec2(0, 4 * S()));

    // Search, category filter and rescan: side by side when there is room, else stacked, so
    // nothing runs into its neighbor.
    {
        const float room = ImGui::GetContentRegionAvail().x, gap = ImGui::GetStyle().ItemSpacing.x;
        const float rescanW = ImGui::CalcTextSize("Rescan devices").x + 2 * ImGui::GetStyle().FramePadding.x + 8 * S();
        const float categoryW = 240 * S();
        const bool row = room >= 180 * S() + categoryW + rescanW + 2 * gap + 40 * S();
        ImGui::SetNextItemWidth(row ? room - categoryW - rescanW - 2 * gap : room);
        ImGui::InputTextWithHint("##device-search", "Search by name, brand or ID", ui.deviceFilter, sizeof ui.deviceFilter);
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        if (row) ImGui::SameLine();
        ImGui::SetNextItemWidth(row ? categoryW : std::min(room, categoryW));
        const char* selected = ui.deviceCategory < 0 ? "All categories" : inventory::Name(static_cast<inventory::Category>(ui.deviceCategory));
        if (ImGui::BeginCombo("##device-category", selected)) {
            if (ImGui::Selectable("All categories", ui.deviceCategory < 0)) ui.deviceCategory = -1;
            for (int i = 0; i < static_cast<int>(inventory::Category::Count); ++i)
                if (ImGui::Selectable(inventory::Name(static_cast<inventory::Category>(i)), ui.deviceCategory == i)) ui.deviceCategory = i;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (row) ImGui::SameLine();
        if (Btn(ctl.presenceScanning() ? "Scanning..." : "Rescan devices")) ctl.RescanDevices();
    }

    int shown = 0;
    for (const auto& d : devices)
        if ((ui.deviceCategory < 0 || ui.deviceCategory == static_cast<int>(d.category)) && inventory::Matches(d, ui.deviceFilter)) ++shown;
    Muted("Showing %d of %d. Click a device for details.", shown, static_cast<int>(devices.size()));
    if (!shown && ctl.presence().scanned) Muted(devices.empty() ? "Windows did not report any devices." : "No devices match your search.");
    ImGui::Dummy(ImVec2(0, 2 * S()));

    const bool narrow = ImGui::GetContentRegionAvail().x < 650 * S();
    const bool filtering = ui.deviceFilter[0] || ui.deviceCategory >= 0;
    for (int i = 0; i < static_cast<int>(inventory::Category::Count); ++i) {
        if (ui.deviceCategory >= 0 && ui.deviceCategory != i) continue;
        const auto category = static_cast<inventory::Category>(i);
        int count = 0, bad = 0;
        for (const auto& d : devices)
            if (d.category == category && inventory::Matches(d, ui.deviceFilter)) { ++count; if (d.problem) ++bad; }
        if (!count) continue;
        ImGui::PushID(i);
        ImGui::SetNextItemOpen(filtering, filtering ? ImGuiCond_Always : ImGuiCond_Once);
        // The header draws its own label: icon, name, then the count (and a warning) on the right,
        // each at a measured spot so none can run into another.
        const ImVec2 head = ImGui::GetCursorScreenPos();
        const bool open = ImGui::CollapsingHeader("##category", ImGuiTreeNodeFlags_AllowOverlap);
        const bool headHovered = ImGui::IsItemHovered();
        if (headHovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float h = ImGui::GetFrameHeight(), icon = 16 * S();
            float x = head.x + ImGui::GetTreeNodeToLabelSpacing() + 4 * S();
            DrawIcon(HardwareIcon(category), ImVec2(x + icon / 2, head.y + h / 2), icon, Hex(bad ? kAmber : headHovered ? kText : kMuted));
            x += icon + 10 * S();
            const char* name = inventory::Name(category);
            const float textY = head.y + (h - ImGui::GetFontSize()) / 2;
            dl->AddText(ImVec2(x, textY), Hex(headHovered ? kText : kMuted), name);
            // Right side: the warning (if any), then the count, right-aligned inside the header.
            float right = head.x + ImGui::GetContentRegionAvail().x - 12 * S();
            char countText[16];
            snprintf(countText, sizeof countText, "%d", count);
            right -= ImGui::CalcTextSize(countText).x;
            dl->AddText(ImVec2(right, textY), Hex(kMuted), countText);
            if (bad) {
                char warn[40];
                snprintf(warn, sizeof warn, "%d %s", bad, bad == 1 ? "needs attention" : "need attention");
                right -= ImGui::CalcTextSize(warn).x + 14 * S();
                // Only when it still clears the name.
                if (right > x + ImGui::CalcTextSize(name).x + 12 * S()) dl->AddText(ImVec2(right, textY), Hex(kAmber), warn);
            }
        }
        if (open && ImGui::BeginTable("hardware", narrow ? 2 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 3.f);
            if (!narrow) ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableHeadersRow();
            for (const auto& d : devices) {
                if (d.category != category || !inventory::Matches(d, ui.deviceFilter)) continue;
                ImGui::PushID(d.id.c_str());
                const float lineH = ImGui::GetTextLineHeight();
                const float rowH = std::max(2 * lineH + 10 * S(), ImGui::GetFrameHeight() + 8 * S());
                ImGui::TableNextRow(0, rowH);
                ImGui::TableNextColumn();
                const ImVec2 cell = ImGui::GetCursorPos();
                // One clickable strip over the whole row; its text is drawn on top and clipped by the cell.
                if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                      ImVec2(0, rowH - 2 * S())))
                    ui.deviceDetail = "hardware:" + d.id;
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("%s\n%s\nClick for details", d.manufacturer.empty() ? "Unknown manufacturer" : d.manufacturer.c_str(),
                                      d.id.c_str());
                }
                ImGui::SetCursorPos(ImVec2(cell.x, cell.y + 3 * S()));
                ImGui::TextUnformatted(d.name.c_str());
                ImGui::SetCursorPosX(cell.x);
                ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));  // one line, clipped (never wrapped into the next row)
                ImGui::TextUnformatted(d.manufacturer.empty() ? "Unknown manufacturer" : d.manufacturer.c_str());
                ImGui::PopStyleColor();
                if (!narrow) {
                    ImGui::TableNextColumn();
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3 * S());
                    ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));
                    ImGui::TextUnformatted(d.className.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::TableNextColumn();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3 * S());
                const auto status = HardwareStatus(d);
                Pill(status.text.c_str(), status.color);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
    EndCard();
}

// A device's own page.
void NzxtCard(Controller& ctl, const Fonts& f);

const sensors::GpuStat* MainGpu(const sensors::SystemSnapshot& snap) { return MainGpuOf(snap); }

void DeviceDetailPage(Controller& ctl, Integrations& in, UiState& ui, const Fonts& f,
                      const sensors::SystemSnapshot& snap) {
    const std::string& id = ui.deviceDetail;
    if (id.rfind("hardware:", 0) == 0) {
        const auto devices = HardwareInventory(ctl, snap);
        const auto found = std::find_if(devices.begin(), devices.end(), [&](const inventory::Device& d) { return "hardware:" + d.id == id; });
        if (found == devices.end()) {
            if (Btn("<  All devices")) ui.deviceDetail.clear();
            Muted("Windows no longer reports this device. Rescan devices to refresh the list.");
            return;
        }
        const auto& d = *found;
        if (!DeviceHeader(ui, f, Icon::Plug, d.name, HardwareStatus(d), false, inventory::Name(d.category))) return;
        BeginCard("hardware-detail");
        CardTitle(f, "Device information", Icon::Info);
        auto line = [&](const char* label, const std::string& value) {
            if (value.empty()) return;
            Muted("%s", label);
            ImGui::TextWrapped("%s", value.c_str());
            ImGui::Dummy(ImVec2(0, 3 * S()));
        };
        line("Type", d.className);
        line("Category", inventory::Name(d.category));
        line("Manufacturer", d.manufacturer);
        line("Location", d.location);
        line("Device ID", d.id);
        if (d.vid) {
            char ids[48];
            snprintf(ids, sizeof ids, "%04X:%04X", d.vid, d.pid);
            line("Vendor and product ID", ids);
        }
        if (d.problem) Muted("Windows reports problem code %u. Open this device in Windows Device Manager for details.", d.problem);
        else if (d.id.rfind("firmware:", 0) != 0 && !d.statusKnown) Muted("Windows lists this device as connected, but its driver status was not available.");
        Muted("Lighting support is listed on the Lighting and readings tab.");
        EndCard();
        return;
    }
    if (id == "aio") {
        const catalog::AioModel* aio = ctl.presence().Aio();
        if (!aio) {
            if (Btn("<  All devices")) ui.deviceDetail.clear();
            Muted("The cooler isn't there any more. Rescan devices on the list.");
            return;
        }
        char usb[64];
        snprintf(usb, sizeof usb, "AIO water cooler%s (USB %04X:%04X)", aio->lcd ? " with a screen" : "", aio->vid, aio->pid);
        const nzxt::Status k = ctl.kraken();
        const DeviceStatus st = aio->vid == nzxt::kVid ? (k.valid ? DeviceStatus("Reading", kGreen) : DeviceStatus("Connecting", kMuted))
                                                       : DeviceStatus("Found", kMuted);
        if (!DeviceHeader(ui, f, Icon::Fan, aio->name, st, false, usb)) return;
        if (aio->vid == nzxt::kVid) NzxtCard(ctl, f);
        BeginCard("aio-setup");
        CardTitle(f, "In My setup", Icon::Tower);
        pc::Layout l = view3d::Gather(ctl, snap).layout;
        if (l.cooler == pc::Cooler::Aio) {
            Muted("Shown as your CPU cooler, radiator %s. Change it under Fans and cooling on My setup.",
                  l.radiator == pc::Mount::Top ? "on top" : "in front");
        } else {
            Muted("My setup shows an air cooler.");
            if (Btn("Show this AIO in My setup")) {
                l.cooler = pc::Cooler::Aio;
                SaveLayout(ctl, l);
            }
        }
        Muted("RGB fans connected to a motherboard header or another controller are lit through that connection.");
        EndCard();
        return;
    }
    if (id == "gpu") {
        const sensors::GpuStat* g = MainGpu(snap);
        if (!g) {
            if (Btn("<  All devices")) ui.deviceDetail.clear();
            Muted("No graphics card reported.");
            return;
        }
        std::string detail = "Graphics card";
        if (g->vramTotal) detail += ", " + Gb(g->vramTotal) + " of its own memory";
        if (!DeviceHeader(ui, f, Icon::Gpu, sensors::FriendlyGpu(g->name), DeviceStatus("Detected", kMuted), false, detail)) return;
        BeginCard("gpu-now");
        CardTitle(f, "Now", Icon::Gauge);
        bool any = false;
        auto line = [&](const char* what, double v, const char* fmt) {
            if (v < 0) return;
            any = true;
            ImGui::AlignTextToFramePadding();
            Muted("%s", what);
            ImGui::SameLine(140 * S());
            ImGui::Text(fmt, v);
        };
        line("Load", g->load, "%.0f %%");
        line("Temperature", g->temp, "%.0f \xC2\xB0" "C");
        line("Fan", g->fanPct, "%.0f %%");
        line("Power", g->powerW, "%.0f W");
        line("Clock", g->clockMhz, "%.0f MHz");
        line("Memory clock", g->memClockMhz, "%.0f MHz");
        if (g->vramUsed && g->vramTotal) {
            ImGui::AlignTextToFramePadding();
            Muted("Memory");
            ImGui::SameLine(140 * S());
            ImGui::Text("%s / %s", Gb(g->vramUsed).c_str(), Gb(g->vramTotal).c_str());
            any = true;
        }
        if (!any) Muted("No live readings: they come from NVIDIA's driver (NVIDIA cards only for now).");
        EndCard();
        BeginCard("gpu-rgb");
        CardTitle(f, "Lighting", Icon::Lighting);
        Muted("GPU lighting is available when a connected RGB driver exposes this card. If it has lights that "
              "OpenRGB supports, turn on OpenRGB under Integrations and it appears in the list as its own device.");
        EndCard();
        return;
    }
    if (id.rfind("aura:", 0) == 0) {
        const auto& devs = ctl.devices();
        const AuraDeviceInfo* d = nullptr;
        for (const auto& x : devs)
            if ("aura:" + Utf8(x.name) == id) d = &x;
        if (!d) {
            if (Btn("<  All devices")) ui.deviceDetail.clear();
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
    if (id == device::kController) {
        const PadInput::Snapshot pad = ctl.pad().Get();
        const char* model = pad.connected ? pad::ModelName(pad.state.model) : "DualSense";
        if (!DeviceHeader(ui, f, Icon::Game, model, DualSenseStatus(ctl), true, "By USB cable, or Bluetooth"))
            return;
        PadLiveCard(ctl, f, pad);
        PadBatteryCard(f, pad);
        PadTimingCard(ctl, f, pad);
        PadMappingCard(ctl, f, pad);
        DualSenseCard(ctl, f);
        DeviceLightingCard(ctl, ui, f, device::kController);
        return;
    }
    if (id.rfind("memory-module:", 0) == 0) {
        for (size_t i = 0; i < snap.smbios.memory.size(); ++i) {
            if (id != "memory-module:" + std::to_string(i)) continue;
            const auto& m = snap.smbios.memory[i];
            const std::string name = sensors::FriendlyMemory(m.manufacturer, m.part);
            if (!DeviceHeader(ui, f, Icon::Memory, name, DeviceStatus("Detected", kGreen), false,
                              "Memory module, " + m.slot)) return;
            BeginCard("memory-module");
            CardTitle(f, "Module", Icon::Memory);
            if (!m.slot.empty()) Muted("Slot: %s", m.slot.c_str());
            if (!m.bank.empty()) Muted("Bank: %s", m.bank.c_str());
            Muted("Manufacturer: %s", m.manufacturer.c_str());
            Muted("Part number: %s", m.part.c_str());
            ImGui::Text("%.1f GB, %u MT/s", m.sizeMb / 1024.0, m.speedMts);
            Muted("Detected from motherboard firmware. This confirms the installed module; lighting support "
                  "is confirmed by a lighting connection.");
            if (HasRgbRam(ctl, snap)) {
                if (Btn("Native RAM lighting settings")) ui.deviceDetail = device::kRam;
            }
            Muted("For other RGB memory families and chipsets, use OpenRGB's SDK server. Detected RGB memory "
                  "appears as a separate lighting device with its own switch.");
            if (Btn("Open lighting connections")) { ui.page = Page::Integrations; ui.deviceDetail.clear(); }
            EndCard();
            return;
        }
        if (Btn("<  All devices")) ui.deviceDetail.clear();
        Muted("This memory module is no longer reported.");
        return;
    }
    if (id.rfind("lamparray:", 0) == 0) {
        const std::string identity = id.substr(10);
        for (const auto& d : ctl.lampArray().devices()) {
            if (d.id != identity) continue;
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
                if (!ctl.prefs().lampArray && Btn("Enable Windows lighting")) ctl.SetLampArrayEnabled(true);
                bool on = ctl.LampArrayOn(d);
                if (Toggle("Light it", &on)) {
                    ctl.prefs().lampArrayDevices[d.id] = on;
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
        if (Btn("<  All devices")) ui.deviceDetail.clear();
        Muted("This device isn't there any more.");
        return;
    }
    if (id.rfind("openrgb:", 0) == 0) {
        const std::string identity = id.substr(8);
        for (const auto& d : ctl.openRgb().devices()) {
            if (d.id != identity) continue;
            const std::string detail = std::string(openrgb::TypeName(d.type)) + (d.vendor.empty() ? "" : " by " + d.vendor) +
                                       ", " + std::to_string(d.leds) + " LEDs, through OpenRGB";
            if (!DeviceHeader(ui, f, Icon::Leds, d.name, OpenRgbStatus(ctl, d), false, detail)) return;
            BeginCard("openrgb-device");
            CardTitle(f, "Settings", Icon::Gear);
            bool on = ctl.OpenRgbOn(d);
            if (!ctl.prefs().openRgb && Btn("Enable OpenRGB lighting")) ctl.SetOpenRgbEnabled(true);
            if (!d.location.empty()) Muted("Location: %s", d.location.c_str());
            if (!d.serial.empty()) Muted("Serial: %s", d.serial.c_str());
            if (Toggle("Light it through OpenRGB", &on)) {
                ctl.prefs().openRgbDevices[d.id] = on;
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
        if (Btn("<  All devices")) ui.deviceDetail.clear();
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
    CardTitle(f, "Lighting devices", Icon::Leds);
    const int lit = LitDevices(ctl);
    if (ctl.output().stopped) {
        Pill("Not controlling the lights", kMuted);
        Muted("LumaBridge takes over when a game or your manual lighting needs the lights.");
    } else if (lit) {
        Pill("Following LumaBridge", kGreen);
        ImGui::SameLine();
        Muted("%d lighting device%s under LumaBridge control.", lit, lit == 1 ? "" : "s");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Includes Aura, RAM, NZXT, peripherals, Windows lighting and OpenRGB.\nFans on one header and RAM sticks controlled together count as one group.");
    } else {
        Pill("Waiting for devices", kAmber);
        Muted("No lighting devices are following LumaBridge right now. Check their status below.");
    }
    if (st.running && !st.connected)
        Muted("The Aura controller wasn't found. Other lighting devices can still work; details are in the log.");
    ImGui::Dummy(ImVec2(0, 4 * S()));
    ImGui::BeginDisabled(ctl.presenceScanning());
    if (Btn(ctl.presenceScanning() ? "Scanning..." : "Rescan devices")) ctl.RescanDevices();
    ImGui::EndDisabled();
    EndCard();

    // The two views, as the same pill switch the Lighting page uses for 2D / 3D.
    if (ui.devicesLanding) ui.devicesTab = 0;
    ui.devicesLanding = false;
    ImGui::Dummy(ImVec2(0, 4 * S()));
    {
        static const char* const kTabs[] = {"Lighting and readings", "All hardware"};
        const ImVec2 at = ImGui::GetCursorScreenPos();
        // Segments are equal width, so both are as wide as the longer label needs.
        float longest = 0;
        for (const char* t : kTabs) longest = std::max(longest, ImGui::CalcTextSize(t).x);
        const float width = 6 * S() + 2 * (longest + 32 * S());
        Segmented("device-pages", &ui.devicesTab, kTabs, 2, width);
        const ImVec2 after = ImGui::GetCursorScreenPos();
        // The hint sits beside the switch (drawn, since Segmented leaves a spacer after itself).
        const float hintX = at.x + width + 14 * S();
        const char* hint = ui.devicesTab == 0 ? "Devices LumaBridge can light or read." : "Everything Windows reports on this PC.";
        if (hintX + ImGui::CalcTextSize(hint).x < at.x + ImGui::GetContentRegionAvail().x)
            ImGui::GetWindowDrawList()->AddText(ImVec2(hintX, at.y + (ImGui::GetFrameHeight() + 4 * S() - ImGui::GetFontSize()) / 2),
                                                Hex(kMuted), hint);
        ImGui::SetCursorScreenPos(after);
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (ui.devicesTab == 0) {
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
            rows.push_back({device::kRam, "Native RAM lighting", hw.ramName.empty() ? "Memory" : hw.ramName, Icon::Memory,
                            RamStatus(ctl, setup && setup->state == IntegrationState::Active),
                            ctl.prefs().ramLighting && ctl.hardware().sticks() ? ctl.hardware().sticks() * 5 : -1});
        }
        for (size_t i = 0; i < snap.smbios.memory.size(); ++i) {
            const auto& m = snap.smbios.memory[i];
            const std::string name = sensors::FriendlyMemory(m.manufacturer, m.part) +
                                     (m.slot.empty() ? " #" + std::to_string(i + 1) : " (" + m.slot + ")");
            rows.push_back({"memory-module:" + std::to_string(i), name,
                            "Memory, " + std::to_string(m.sizeMb / 1024) + " GB, " + std::to_string(m.speedMts) + " MT/s",
                            Icon::Memory, DeviceStatus("Detected", kGreen, "Module information from motherboard firmware")});
        }
        if (HasLogitechRgb(ctl))
            rows.push_back({device::kMouse, LogitechName(ctl), LogitechKinds(ctl) + ", through G HUB", Icon::Mouse,
                            LogitechStatus(ctl)});
        // Shown whenever switched on, found or not: otherwise a device that fails to be found has
        // no row to click for why (the exact diagnostic a "not found" state exists to answer).
        if (ctl.prefs().azothKeyboard || HasAzoth(ctl))
            rows.push_back({device::kKeyboard, "ASUS ROG Azoth", "Keyboard", Icon::Keyboard, AzothStatus(ctl)});
        if (ctl.prefs().dualsenseController || HasDualSense(ctl))
            rows.push_back({device::kController, "DualSense", "Controller", Icon::Game, DualSenseStatus(ctl)});
        for (const auto& d : ctl.lampArray().devices())
            rows.push_back({"lamparray:" + d.id, d.name, std::string(lamparray::KindName(d.kind)) + ", Windows lighting standard",
                                Icon::Leds, LampArrayStatus(ctl, d), d.lamps ? static_cast<int>(d.lamps) : -1});
        const auto openRgbDevices = ctl.openRgb().devices();
        size_t openRgbIndex = 0;
        for (const auto& d : openRgbDevices) {
            ++openRgbIndex;
            const bool sameName = std::count_if(openRgbDevices.begin(), openRgbDevices.end(), [&](const OpenRgbDevice& other) {
                return other.name == d.name;
            }) > 1;
            const std::string label = d.name + (sameName ? " (" + (d.serial.empty() ? std::to_string(openRgbIndex) : d.serial) + ")" : "");
            const Icon icon = d.type == 1 ? Icon::Memory : d.type == 2 ? Icon::Gpu : d.type == 3 ? Icon::Fan :
                              d.type == 5 ? Icon::Keyboard : d.type == 6 ? Icon::Mouse : d.type == 10 ? Icon::Game : Icon::Leds;
            rows.push_back({"openrgb:" + d.id, label, std::string(openrgb::TypeName(d.type)) + ", through OpenRGB",
                            icon, OpenRgbStatus(ctl, d), static_cast<int>(d.leds)});
        }
        // The AIO cooler and the graphics card: listed with their readings, lit or not.
        if (const catalog::AioModel* aio = ctl.presence().Aio()) {
            const nzxt::Status k = ctl.kraken();
            DeviceStatus st = aio->vid == nzxt::kVid ? (k.valid ? DeviceStatus("Reading", kGreen, "Liquid temperature, pump and fan speeds")
                                                                : DeviceStatus("Connecting", kMuted, "Reading its status..."))
                                                     : DeviceStatus("Found", kMuted, "Shown as your CPU cooler in My setup");
            rows.push_back({"aio", aio->name, std::string("AIO water cooler") + (aio->lcd ? ", with a screen" : ""), Icon::Fan, st});
        }
        if (const sensors::GpuStat* g = MainGpu(snap))
            rows.push_back({"gpu", sensors::FriendlyGpu(g->name), "Graphics card", Icon::Gpu,
                            DeviceStatus("Detected", kMuted, "Readings on its page; a connected RGB driver confirms lighting support")});
        auto rowCategory = [](const Row& r) {
            if (r.id.rfind("aura:", 0) == 0) return 0;
            if (r.id == "aio" || r.icon == Icon::Fan) return 3;
            if (r.id == "gpu" || r.icon == Icon::Gpu) return 4;
            if (r.icon == Icon::Memory) return 1;
            if (r.icon == Icon::Mouse || r.icon == Icon::Keyboard || r.icon == Icon::Game) return 2;
            if (r.kind.find("Keyboard") != std::string::npos || r.kind.find("Mouse") != std::string::npos ||
                r.kind.find("Headset") != std::string::npos || r.kind.find("Controller") != std::string::npos) return 2;
            return 0;
        };
        std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) { return rowCategory(a) < rowCategory(b); });
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
        // Below 520, Kind and LEDs drop off (Status widens to take the room): five columns of text
        // get cramped before the sidebar even has to collapse.
        const bool devicesNarrow = ImGui::GetContentRegionAvail().x < 520 * S();
        if (rows.empty()) Muted("Nothing LumaBridge can light was found on this PC yet.");
        else if (ImGui::BeginTable("devices", devicesNarrow ? 3 : 5,
                                   ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 2.6f);
            if (!devicesNarrow) ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, devicesNarrow ? 2.2f : 1.8f);
            if (!devicesNarrow) ImGui::TableSetupColumn("LEDs", ImGuiTableColumnFlags_WidthFixed, 50 * S());
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24 * S());
            ImGui::TableHeadersRow();
            int n = 0, lastCategory = -1;
            static const char* categories[] = {"Fan and RGB controllers", "Memory", "Peripherals", "Cooling", "Graphics"};
            for (const Row& r : rows) {
                const int category = rowCategory(r);
                if (category != lastCategory) {
                    ImGui::TableNextRow(0, 32 * S());
                    ImGui::TableNextColumn();
                    ImGui::PushFont(f.bold);
                    ImGui::TextColored(V4(kAccent), "%s", categories[category]);
                    ImGui::PopFont();
                    lastCategory = category;
                }
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
                if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SameLine(0, 0);
                ImGui::AlignTextToFramePadding();  // the icon and name on the row's text line
                IconItem(r.icon, 16 * S(), r.status.color == kGreen ? Hex(kAccent) : Hex(kMuted));
                ImGui::SameLine();
                ImGui::TextUnformatted(r.name.c_str());
                if (!devicesNarrow) {
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    Muted("%s", r.kind.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                Pill(r.status.text.c_str(), r.status.color);
                if (!r.status.tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.status.tip.c_str());
                if (!devicesNarrow) {
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    if (r.leds >= 0) Muted("%d", r.leds);
                }
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
    } else {
        HardwareInventoryCard(ctl, ui, f, snap);
    }
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
    ui.dcsFolders = !DcsSavedGames().empty();
    ui.dcsInstalled = DcsSetUp();
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
    } else if (key == "f1") {
        if (feeds.F1PortBusy()) Pill("Port busy", kRed);
        else Pill(feeds.F1Seen() ? "Receiving" : "Switch on in the game", feeds.F1Seen() ? kGreen : kAmber);
    } else if (key == "beamng") {
        if (feeds.UdpPortBusy(GameFeeds::kBeamNg)) Pill("Port busy", kRed);
        else Pill(feeds.UdpSeen(GameFeeds::kBeamNg) ? "Receiving" : "Switch on in the game", feeds.UdpSeen(GameFeeds::kBeamNg) ? kGreen : kAmber);
    } else if (key == "dirt") {
        if (feeds.UdpPortBusy(GameFeeds::kDirt)) Pill("Port busy", kRed);
        else Pill(feeds.UdpSeen(GameFeeds::kDirt) ? "Receiving" : "Switch on in the game", feeds.UdpSeen(GameFeeds::kDirt) ? kGreen : kAmber);
    } else if (key == "ams2") {
        if (feeds.UdpPortBusy(GameFeeds::kAms2)) Pill("Port busy", kRed);
        else Pill(feeds.UdpSeen(GameFeeds::kAms2) ? "Receiving" : "Switch on in the game", feeds.UdpSeen(GameFeeds::kAms2) ? kGreen : kAmber);
    } else if (key == "xplane") {
        if (feeds.UdpPortBusy(GameFeeds::kXPlane)) Pill("Port busy", kRed);
        else Pill(feeds.UdpSeen(GameFeeds::kXPlane) ? "Receiving" : "Switch on in the game", feeds.UdpSeen(GameFeeds::kXPlane) ? kGreen : kAmber);
    } else if (key == "elite") {
        Pill(feeds.EliteSeen() ? "Receiving" : "Ready", kGreen);
    } else if (key == "msfs") {
        if (feeds.FlightSimSeen()) Pill("Receiving", kGreen);
        else if (feeds.FlightSimDll() == 0) Pill("Needs SimConnect", kAmber);
        else Pill("Ready", kGreen);
    } else if (key == "dcs") {
        if (feeds.DcsPortBusy()) Pill("Port busy", kRed);
        else if (feeds.DcsSeen()) Pill("Receiving", kGreen);
        else Pill(ui.dcsInstalled ? "Set up" : "Not set up", ui.dcsInstalled ? kGreen : kAmber);
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
            if (Btn("Remove##cs2"))
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
            if (on ? Btn("Switch off##rl") : PrimaryButton("Switch on##rl"))
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
            if (Btn("Remove##dota2"))
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
    } else if (key == "f1") {
        Muted("In the game: Settings > Telemetry Settings > UDP Telemetry: On, UDP IP Address: 127.0.0.1, UDP Port: "
              "the port below.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Port");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * S());
        int port = ctl.prefs().f1Port;
        if (ImGui::InputInt("##f1port", &port, 0, 0) && port > 1024 && port < 65536) {
            ctl.prefs().f1Port = port;
            ctl.Changed();
            ctl.RefreshFeedSettings();
        }
        ImGui::SameLine();
        Muted("%s", feeds.F1PortBusy() ? "Taken by another program (a telemetry app?) - pick another port here and in "
                                          "the game."
                    : feeds.F1Seen() ? "Receiving while you drive."
                                     : "Works while you drive, once UDP Telemetry is on.");
    } else if (key == "beamng") {
        Muted("In the game: Options > Other > Protocols (Live for Speed OutGauge) on, IP 127.0.0.1, Port: the port below.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Port");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * S());
        int port = ctl.prefs().beamngPort;
        if (ImGui::InputInt("##beamngport", &port, 0, 0) && port > 1024 && port < 65536) {
            ctl.prefs().beamngPort = port;
            ctl.Changed();
            ctl.RefreshFeedSettings();
        }
        ImGui::SameLine();
        Muted("%s", feeds.UdpPortBusy(GameFeeds::kBeamNg) ? "Taken by another program (a telemetry app?) - pick another port here and in the game."
                    : feeds.UdpSeen(GameFeeds::kBeamNg) ? "Receiving while you play."
                                                       : "Works while you play, once the telemetry is on.");
    } else if (key == "dirt") {
        Muted("Close the game, open Documents\\My Games\\DiRT Rally 2.0\\hardwaresettings\\hardware_settings_config.xml (DiRT Rally: the DiRT Rally folder) and set the udp line to: enabled=\"true\" extradata=\"3\" ip=\"127.0.0.1\" port=\"the port below\" delay=\"1\". Needs a different port than F1 uses if both are on.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Port");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * S());
        int port = ctl.prefs().dirtPort;
        if (ImGui::InputInt("##dirtport", &port, 0, 0) && port > 1024 && port < 65536) {
            ctl.prefs().dirtPort = port;
            ctl.Changed();
            ctl.RefreshFeedSettings();
        }
        ImGui::SameLine();
        Muted("%s", feeds.UdpPortBusy(GameFeeds::kDirt) ? "Taken by another program (a telemetry app?) - pick another port here and in the game."
                    : feeds.UdpSeen(GameFeeds::kDirt) ? "Receiving while you play."
                                                       : "Works while you play, once the telemetry is on.");
    } else if (key == "ams2") {
        Muted("In the game: Options > System > Shared Memory: Project CARS 2 UDP, UDP Frequency: 1 or higher. It sends to port 5606; change the port below only if you changed it in the game.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Port");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * S());
        int port = ctl.prefs().ams2Port;
        if (ImGui::InputInt("##ams2port", &port, 0, 0) && port > 1024 && port < 65536) {
            ctl.prefs().ams2Port = port;
            ctl.Changed();
            ctl.RefreshFeedSettings();
        }
        ImGui::SameLine();
        Muted("%s", feeds.UdpPortBusy(GameFeeds::kAms2) ? "Taken by another program (a telemetry app?) - pick another port here and in the game."
                    : feeds.UdpSeen(GameFeeds::kAms2) ? "Receiving while you play."
                                                       : "Works while you play, once the telemetry is on.");
    } else if (key == "xplane") {
        Muted("In the sim: Settings > Data Output: tick the Network via UDP box for rows 3 (Speeds) and 4 (Mach, VVI, G-load); in Settings > Network set the IP for data output to 127.0.0.1 and the port below.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Port");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * S());
        int port = ctl.prefs().xplanePort;
        if (ImGui::InputInt("##xplaneport", &port, 0, 0) && port > 1024 && port < 65536) {
            ctl.prefs().xplanePort = port;
            ctl.Changed();
            ctl.RefreshFeedSettings();
        }
        ImGui::SameLine();
        Muted("%s", feeds.UdpPortBusy(GameFeeds::kXPlane) ? "Taken by another program (a telemetry app?) - pick another port here and in the game."
                    : feeds.UdpSeen(GameFeeds::kXPlane) ? "Receiving while you play."
                                                       : "Works while you play, once the telemetry is on.");
    } else if (key == "elite") {
        Muted("%s", feeds.EliteSeen() ? "Elite Dangerous is writing its ship status."
                                      : "Nothing to set up: it works once the game is running (the game's own Status.json in "
                                        "Saved Games).");
    } else if (key == "msfs") {
        if (feeds.FlightSimSeen())
            Muted("Flight Simulator is sending your aircraft's state.");
        else if (feeds.FlightSimDll() == 0)
            Muted("LumaBridge talks to the sim through SimConnect, whose SimConnect.dll comes with Microsoft's free "
                  "Flight Simulator SDK. In the sim: Options > General Options > Developers > Developer Mode on, "
                  "then in the Developer menu: Help > SDK Installer. Or copy SimConnect.dll next to LumaBridge.exe. "
                  "LumaBridge finds it by itself; nothing to set up in the sim.");
        else
            Muted("%s", feeds.FlightSimConnected() ? "Connected. It lights up once you're in a flight."
                                                   : "Nothing to set up: it connects while the sim runs.");
    } else if (key == "dcs") {
        if (!ui.dcsFolders) {
            Muted("DCS World's Saved Games folder wasn't found. Start DCS once, then come back.");
        } else if (ui.dcsInstalled) {
            Muted("%s", feeds.DcsPortBusy() ? "UDP port 49717 is taken by another program."
                        : feeds.DcsSeen() ? "DCS is sending your aircraft's state."
                                          : "Set up. It lights up in your next mission (restart DCS if it's running).");
            if (Btn("Remove##dcs")) {
                const std::string err = DcsSetUpScripts(true);
                ui.feedMessageId = "dcs";
                ui.feedMessage = err.empty() ? "Done: DCS World export removed" : "Failed: " + err;
                ui.feedCheckAt = 0;
            }
        } else {
            if (PrimaryButton("Set up##dcs")) {
                const std::string err = DcsSetUpScripts(false);
                ui.feedMessageId = "dcs";
                ui.feedMessage = err.empty() ? "Done: DCS World export set up - restart DCS if it's running"
                                             : "Failed: " + err;
                ui.feedCheckAt = 0;
            }
            ImGui::SameLine();
            Muted("Adds LumaBridge.lua and one line to Export.lua in Saved Games\\DCS\\Scripts (DCS's own place "
                  "for export scripts; other scripts there keep working).");
        }
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

    if (Btn("<  All games")) {
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
        if (Btn("Remove from the list")) {
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

    RefreshFeeds(ctl, ui);

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
    if (Btn(ctl.libraryScanning() ? "Scanning..." : "Rescan")) ctl.RescanLibrary();
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
    // Below 560, Store and Without game lighting drop off (open the row for those); Dynamic
    // lighting widens to take the room.
    const bool gamesNarrow = ImGui::GetContentRegionAvail().x < 560 * S();
    if (!rows.empty() &&
        ImGui::BeginTable("games", gamesNarrow ? 3 : 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 2.6f);
        if (!gamesNarrow) ImGui::TableSetupColumn("Store", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Dynamic lighting", ImGuiTableColumnFlags_WidthStretch, gamesNarrow ? 2.4f : 1.8f);
        if (!gamesNarrow) ImGui::TableSetupColumn("Without game lighting", ImGuiTableColumnFlags_WidthStretch, 1.6f);
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
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", ToUtf8(g.exePath.empty() ? g.dir : g.exePath).c_str());
            }
            ImGui::SameLine(0, 0);
            ImGui::AlignTextToFramePadding();
            if (row.running) ImGui::PushFont(f.bold);
            ImGui::TextUnformatted(name.c_str());
            if (row.running) ImGui::PopFont();
            if (row.running) {
                ImGui::SameLine();
                Pill("Running", kAccent);
            }
            if (!gamesNarrow) {
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                Muted("%s", ToUtf8(g.store).c_str());
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const games::GameProfile* profile = ProfileFor(&g, row.running, name);
            const LightingStatus st = GameLighting(ctl, g, row.running, profile);
            if (st.pill) Pill(st.text, st.color);
            else Muted("%s", st.text);
            if (!st.tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.tip.c_str());
            if (!gamesNarrow) {
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

    // Built-in game lighting (below the list): the games LumaBridge lights through their official data.
    ImGui::Dummy(ImVec2(0, 8 * S()));
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted("Built-in game lighting");
    ImGui::PopFont();
    Muted("These games light up through their own official data, no vendor software needed. Click one to set it up.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    static const char* kBuiltIn[] = {"cs2", "rocketleague", "warthunder", "dota2", "league", "forza", "f1", "beamng", "dirt", "ams2", "xplane", "elite", "msfs", "dcs"};
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
            if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
}

// An NZXT Kraken AIO: its own readings (read only; NZXT CAM keeps its lighting and screen).
void NzxtCard(Controller& ctl, const Fonts& f) {
    const catalog::AioModel* aio = ctl.presence().Aio();
    if (!aio || aio->vid != nzxt::kVid) return;
    BeginCard("nzxt");
    IconItem(Icon::Fan, 18 * S(), Hex(kAccent));
    ImGui::SameLine(0, 10 * S());
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted(aio->name);
    ImGui::PopFont();
    ImGui::SameLine();
    const nzxt::Status k = ctl.kraken();
    const nzxt::KrakenState st = ctl.krakenState();
    if (k.valid) Pill("Reading", kGreen);
    else if (st == nzxt::KrakenState::CantOpen || st == nzxt::KrakenState::NoReply) Pill("No reply", kAmber);
    else Pill("Connecting", kMuted);
    char pid[64];
    snprintf(pid, sizeof pid, "USB 1E71:%04X%s", aio->pid, aio->lcd ? ", with a screen" : "");
    Muted("%s", pid);
    if (k.valid) {
        ImGui::Text("Liquid %.1f \xC2\xB0" "C    Pump %d RPM (%d%%)", k.liquidC, k.pumpRpm, k.pumpDuty);
        if (k.fanRpm > 0) ImGui::Text("Radiator fans %d RPM (%d%%)", k.fanRpm, k.fanDuty);
    } else if (st == nzxt::KrakenState::CantOpen) {
        const unsigned long err = ctl.krakenError();
        Muted("Windows won't let LumaBridge open it (error %lu%s). Details are in the log (Settings).", err,
              err == 32 ? ": another app has it to itself, most likely NZXT CAM" : err == 5 ? ": access denied" : "");
    } else if (st == nzxt::KrakenState::NoReply) {
        Muted(ctl.krakenListening()
                  ? "NZXT CAM has the Kraken open, so LumaBridge listens to the readings CAM asks for: none came yet. "
                    "With CAM running (even in the tray) they appear here."
                  : "It isn't answering status requests; LumaBridge keeps trying. The log (Settings) lists its interfaces.");
    }
    if (k.valid && ctl.krakenListening()) Muted("Read alongside NZXT CAM (listening to the readings it asks for).");
    Muted("Liquid temperature and pump and fan speeds show on the dashboard and in My setup.");
    if (nzxt::LightingChannels(aio->pid)) {
        bool on = ctl.prefs().nzxtLighting;
        if (Toggle("Light the Kraken (experimental)", &on)) {
            ctl.prefs().nzxtLighting = on;
            ctl.Changed();
        }
        Muted(aio->pid == 0x3008 ? "Lights accessories on the Kraken's NZXT RGB connector. The LCD stays with CAM."
                                 : "Lights the pump ring, logo and attached NZXT RGB accessories together.");
        if (on) {
            if (ctl.krakenLightingActive()) Pill("Following LumaBridge", kGreen);
            else if (ctl.krakenLightingError()) Muted("Lighting unavailable (Windows error %lu). Close CAM's lighting control and retry.",
                                                    ctl.krakenLightingError());
            else Muted("Waiting for lighting access...");
        }
        Muted("Uses documented lighting commands; these models still need hardware testing. When disabled, "
              "LumaBridge stops sending colors; CAM can take over, otherwise the last color remains.");
    } else {
        Muted("This model has no supported RGB channel on its pump USB connection. Its RGB fans may be connected "
              "to the motherboard's ARGB header or a separate RGB controller; use that device's lighting settings.");
    }
    Muted("Fan and pump curves and the LCD stay with NZXT CAM.");
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
    if (st == OpenRgbOutput::State::Connected) Pill(pill, kGreen);
    else Pill("Waiting for SDK server", kMuted);
    Muted("Adds support for RGB memory, coolers, graphics cards, keyboards, mice, fans and controllers across "
          "many brands. Run OpenRGB, open its SDK Server tab and Start Server. Devices appear automatically "
          "even while lighting control is off. Enable control below to make them follow LumaBridge.");
    Muted("Working native connections take priority. Unsupported or unavailable native connections can use "
          "OpenRGB instead. Each device has its own switch on Devices.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    bool on = ctl.prefs().openRgb;
    if (Toggle("Lighting control", &on)) ctl.SetOpenRgbEnabled(on);
    ImGui::SameLine(0, 24 * S());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Port");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * S());
    int port = ctl.prefs().openRgbPort;
    if (ImGui::InputInt("##orgbport", &port, 0, 0) && port > 0 && port < 65536) {
        ctl.SetOpenRgbPort(port);
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
            if (Btn("Remove from a game...")) {
                std::wstring dir = PickFolder(hwnd, L"Choose the game's folder");
                if (!dir.empty()) in.Remove("corsair", dir);
            }
        } else if ((it.id == "handback" || it.id == "helper") && it.state != IntegrationState::NotInstalled) {
            // Setting it up again replaces the task (needed after updates that change it).
            if (it.state == IntegrationState::Problem ? PrimaryButton("Update") : Btn("Set up again"))
                in.Install(it.id);
            ImGui::SameLine();
            if (Btn("Remove")) in.Remove(it.id);
        } else if (it.state == IntegrationState::Active) {
            if (Btn("Remove")) in.Remove(it.id);
        } else if (it.state == IntegrationState::Conflict) {
            if (Btn("Replace vendor runtime")) in.Install(it.id, L"", true);
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
    NzxtCard(ctl, f);

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
    if (Btn("Reset calibration")) {
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
    if (Toggle("Closing the window keeps LumaBridge in the tray", &ctl.prefs().closeToTray)) ctl.Changed();
    Muted(ctl.prefs().closeToTray ? "Close hides the window; games keep their lighting. Exit is in the tray icon's menu."
                                  : "Close exits LumaBridge (the lights go back to Armoury Crate). Minimize keeps it running.");
    EndCard();

    BeginCard("guide");
    CardTitle(f, "Setup guide", Icon::Plug);
    Muted("Asks which RGB hardware and lighting software you have, and sets up LumaBridge's connections to match.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (Btn("Run the setup guide again")) {
        ui.setupStep = 0;
        ui.setupDetected = false;
    }
    EndCard();

    BeginCard("about");
    CardTitle(f, "About", Icon::Info);
    ImGui::Text("LumaBridge %s", kVersionText);
    Muted("Game lighting for ASUS Aura, without Armoury Crate in the way.");
    ImGui::Dummy(ImVec2(0, 2 * S()));
    if (Btn("Releases on GitHub"))
        ShellExecuteW(nullptr, L"open", L"https://github.com/EmilB04/LumaBridge/releases", nullptr, nullptr, SW_SHOWNORMAL);
    EndCard();

    BeginCard("trouble");
    CardTitle(f, "Troubleshooting", Icon::Info);
    if (Btn("Open log folder")) {
        std::wstring dir = ctl.logPath().substr(0, ctl.logPath().find_last_of(L"\\/"));
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (Btn("Open settings file"))
        ShellExecuteW(nullptr, L"open", L"notepad.exe", ctl.configPath().c_str(), nullptr, SW_SHOWNORMAL);
    EndCard();
}

// ---- Frame ---------------------------------------------------------------------------

void Sidebar(Controller& ctl, UiState& ui, const Fonts& f, float width, bool collapsed) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(kSidebar));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, collapsed ? ImVec2(10 * S(), 20 * S()) : ImVec2(16 * S(), 20 * S()));
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
        if (!collapsed) {
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
    }

    // Collapse / expand: a small chevron row, same style as the nav items below.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 28 * S();
        const bool clicked = ImGui::InvisibleButton("collapse", ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (clicked) ui.sidebarCollapsed = !ui.sidebarCollapsed;
        if (collapsed && hovered) ImGui::SetTooltip(ui.sidebarCollapsed ? "Expand" : "Collapse");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hovered) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Hex(kCardHover), 8 * S());
        // Right (tap to expand) when collapsed, left (tap to collapse) when open.
        const ImVec2 c(p.x + h / 2, p.y + h / 2);
        const float len = 4 * S(), dir = collapsed ? -1.f : 1.f, lw = std::max(1.5f, 2 * S());
        const ImU32 col = hovered ? Hex(kText) : Hex(kMuted);
        dl->AddLine(ImVec2(c.x + dir * len, c.y - len), ImVec2(c.x - dir * len * 0.2f, c.y), col, lw);
        dl->AddLine(ImVec2(c.x - dir * len * 0.2f, c.y), ImVec2(c.x + dir * len, c.y + len), col, lw);
        if (!collapsed) dl->AddText(ImVec2(p.x + h, p.y + (h - ImGui::GetTextLineHeight()) / 2), Hex(kMuted), "Collapse");
    }
    ImGui::Dummy(ImVec2(0, collapsed ? 10 * S() : 14 * S()));

    const struct {
        Page page;
        const char* label;
        Icon icon;
    } items[] = {{Page::Dashboard, "Dashboard", Icon::Grid},        {Page::MySetup, "My setup", Icon::Tower},
                 {Page::Lighting, "Lighting", Icon::Lighting},      {Page::GamesList, "Games List", Icon::Game},
                 {Page::Devices, "Devices", Icon::Leds},            {Page::Integrations, "Integrations", Icon::Plug},
                 {Page::Settings, "Settings", Icon::Gear}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& it : items) {
        const bool sel = ui.page == it.page;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 40 * S();
        ImGui::PushID(it.label);
        if (ImGui::InvisibleButton("nav", ImVec2(w, h))) ui.page = it.page;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
        if (sel) {
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Hex(kAccent, 42), 10 * S());
            dl->AddRectFilled(ImVec2(p.x, p.y + 10 * S()), ImVec2(p.x + 3 * S(), p.y + h - 10 * S()), Hex(kAccentHover), 2 * S());
        } else if (hovered) {
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Hex(kCardHover), 10 * S());
        }
        const float iconX = collapsed ? p.x + w / 2 : p.x + 22 * S();
        DrawIcon(it.icon, ImVec2(iconX, p.y + h / 2), 18 * S(), sel ? Hex(kAccentHover) : hovered ? Hex(kText) : Hex(kMuted));
        if (!collapsed) {
            ImGui::PushFont(sel ? f.bold : f.regular);
            dl->AddText(ImVec2(p.x + 44 * S(), p.y + (h - ImGui::GetFontSize()) / 2), sel || hovered ? Hex(kText) : Hex(kMuted), it.label);
            ImGui::PopFont();
        } else if (hovered) {
            ImGui::SetTooltip("%s", it.label);
        }
        ImGui::Dummy(ImVec2(0, 2 * S()));
    }

    // Live output at the bottom of the sidebar.
    const auto& out = ctl.output();
    const float orbR = collapsed ? 14 * S() : 22 * S();
    const float reserve = collapsed ? orbR * 2 + 16 * S() : orbR * 2 + 76 * S();
    const float bottom = ImGui::GetWindowHeight() - 20 * S();
    ImGui::SetCursorPosY(bottom - reserve);  // leaves room for the version line (when shown)
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float orbX = collapsed ? p.x + ImGui::GetContentRegionAvail().x / 2 : p.x + orbR + 4 * S();
    Orb(ImVec2(orbX, p.y + orbR + 4 * S()), orbR, PreviewColor(out));
    if (collapsed) {
        if (ImGui::IsMouseHoveringRect(ImVec2(orbX - orbR, p.y), ImVec2(orbX + orbR, p.y + orbR * 2 + 8 * S())))
            ImGui::SetTooltip("%s: %s", ctl.prefs().mode == Mode::Auto ? "Auto" : "Manual", out.label.c_str());
        ImGui::Dummy(ImVec2(0, orbR * 2 + 8 * S()));
    } else {
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
    }

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
                  : !ctl.prefs().lampArray     ? " - detected, lighting control off"
                  : ctl.LampArrayOn(d)         ? ", " + std::to_string(d.lamps) + " lamps, Windows lighting standard"
                                               : " - lit by LumaBridge another way"));
    for (const auto& d : ctl.openRgb().devices())
        line(Icon::Leds, ctl.OpenRgbOn(d), d.name,
             std::string(openrgb::TypeName(d.type)) + (!ctl.prefs().openRgb ? " - detected, lighting control off" :
                                                    ctl.OpenRgbOn(d) ? ", through OpenRGB" : " - not selected for OpenRGB control"));
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
            if (n == l.Fans() ? PrimaryButton(b, ImVec2(34 * S(), 0)) : Btn(b, ImVec2(34 * S(), 0))) {
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
            if (n == l.LedsPerFan() ? PrimaryButton(b, ImVec2(34 * S(), 0)) : Btn(b, ImVec2(34 * S(), 0))) {
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
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
            case Conn::DualSense: ctl.SetDualSenseEnabled(on); break;
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
        if (back && Btn("Back", ImVec2(110 * S(), 0))) --ui.setupStep;
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
        if (Btn("Skip - I'll set it up myself")) SetupFinish(ctl, ui);
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

// The Auto / Manual switch in the header: a track with a thumb that glides to the chosen
// side, each side with its icon. Returns true when it changed.
bool ModeSwitch(int* mode, float w) {
    ImGui::PushID("mode-switch");
    const float h = 38 * S(), pad = 4 * S();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("sw", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    if (clicked) {
        // A click on either half picks it (a click on the chosen one does nothing).
        const int want = ImGui::GetIO().MousePos.x >= p.x + w / 2 ? 1 : 0;
        if (want != *mode) {
            *mode = want;
            changed = true;
        }
    }
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    // Thumb position eases towards the chosen side.
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetItemID();
    float t = st->GetFloat(id, static_cast<float>(*mode));
    t += (static_cast<float>(*mode) - t) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);
    st->SetFloat(id, t);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(p.x + w, p.y + h);
    dl->AddRectFilled(p, b, Hex(kTrack), h / 2);
    dl->AddRect(p, b, Hex(hovered ? kAccent : kBorder, hovered ? 140 : 170), h / 2, 0, 1.2f * S());
    const float half = (w - pad * 2) / 2;
    const ImVec2 ta(p.x + pad + t * half, p.y + pad), tb(ta.x + half, b.y - pad);
    const float r = (h - pad * 2) / 2;
    dl->AddRectFilled(ImVec2(ta.x - 2 * S(), ta.y - 2 * S()), ImVec2(tb.x + 2 * S(), tb.y + 2 * S()), Hex(kAccent, 40), r + 2 * S());
    dl->AddRectFilled(ta, tb, Hex(kAccent), r);
    // A soft highlight along the thumb's top, towards the second accent color.
    dl->AddRectFilled(ImVec2(ta.x + r * 0.6f, ta.y + 1 * S()), ImVec2(tb.x - r * 0.6f, ta.y + (tb.y - ta.y) * 0.45f),
                      Hex(kAccent2, 36), r * 0.8f);
    const Icon icons[] = {Icon::Game, Icon::Palette};
    const char* labels[] = {"Auto", "Manual"};
    for (int i = 0; i < 2; ++i) {
        // How much of the thumb sits under this side (1 = all of it): its text turns white.
        const float on = 1.f - std::min(1.f, std::fabs(t - static_cast<float>(i)));
        const ImVec4 off = V4(hovered ? kText : kMuted), white = V4(0xFFFFFF);
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(off.x + (white.x - off.x) * on, off.y + (white.y - off.y) * on,
                                                                off.z + (white.z - off.z) * on, 1.f));
        const float icon = 16 * S(), gap = 7 * S();
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        const float x0 = p.x + pad + i * half + (half - icon - gap - ts.x) / 2, cy = p.y + h / 2;
        DrawIcon(icons[i], ImVec2(x0 + icon / 2, cy), icon, col);
        dl->AddText(ImVec2(x0 + icon + gap, cy - ts.y / 2), col, labels[i]);
    }
    ImGui::PopID();
    return changed;
}

// A small round "?" that explains something when hovered.
void HelpBadge(const char* id, float size, void (*explain)()) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("help", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size / 2, p.y + size / 2);
    if (hovered) dl->AddCircleFilled(c, size / 2, Hex(kAccent, 60), 24);
    dl->AddCircle(c, size / 2 - 0.5f, Hex(hovered ? kAccent : kMuted, hovered ? 255 : 150), 24, 1.3f * S());
    const ImVec2 ts = ImGui::CalcTextSize("?");
    dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), Hex(hovered ? kText : kMuted), "?");
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * S(), 14 * S()));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * S());
        ImGui::PushStyleColor(ImGuiCol_PopupBg, V4(kCard));
        ImGui::PushStyleColor(ImGuiCol_Border, V4(kAccent, 0.5f));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 340 * S());
        explain();
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
    ImGui::PopID();
}

const Fonts* g_fonts = nullptr;  // for help texts drawn from plain callbacks

void ExplainModes() {
    auto section = [](Icon icon, const char* title, const char* text) {
        IconItem(icon, 16 * S(), Hex(kAccent));
        ImGui::SameLine();
        if (g_fonts) ImGui::PushFont(g_fonts->bold);
        ImGui::TextUnformatted(title);
        if (g_fonts) ImGui::PopFont();
        Muted("%s", text);
    };
    section(Icon::Game, "Auto",
            "Your games drive the lights: team colors, health, bombs, kills and more, on every device at once. "
            "Between games your idle choice shows (Lighting page): your own look, a rainbow, off, or Armoury Crate.");
    ImGui::Dummy(ImVec2(0, 6 * S()));
    section(Icon::Palette, "Manual",
            "Your own look on every device, all the time: pick an effect and colors on the Lighting page, for all "
            "devices or each one. Games don't change it.");
}

// ---- Notification center ---------------------------------------------------------------------

// Whether a built-in game feed has data, and its port if one can be taken by another program.
struct FeedFact {
    bool busy = false, seen = false, needsSetup = false;
    int port = 0;
};

FeedFact FeedFactOf(const GameFeeds& fd, const Prefs& p, games::Feed feed) {
    using F = games::Feed;
    FeedFact r;
    switch (feed) {
    case F::Cs2Gsi: r = {!fd.Cs2Listening(), fd.Cs2Seen(), true, GameFeeds::kCs2Port}; break;
    case F::Dota2Gsi: r = {!fd.Cs2Listening(), fd.Dota2Seen(), true, GameFeeds::kCs2Port}; break;
    case F::RocketLeagueStats: r.seen = fd.RocketLeagueConnected(); r.needsSetup = true; break;
    case F::WarThunderApi: r.seen = fd.WarThunderSeen(); break;
    case F::LeagueLiveClient: r.seen = fd.LeagueSeen(); break;
    case F::ForzaDataOut: r = {fd.ForzaPortBusy(), fd.ForzaSeen(), true, p.forzaPort}; break;
    case F::F1Telemetry: r = {fd.F1PortBusy(), fd.F1Seen(), true, p.f1Port}; break;
    case F::BeamNgOutGauge: r = {fd.UdpPortBusy(GameFeeds::kBeamNg), fd.UdpSeen(GameFeeds::kBeamNg), true, p.beamngPort}; break;
    case F::DirtRallyUdp: r = {fd.UdpPortBusy(GameFeeds::kDirt), fd.UdpSeen(GameFeeds::kDirt), true, p.dirtPort}; break;
    case F::Ams2Udp: r = {fd.UdpPortBusy(GameFeeds::kAms2), fd.UdpSeen(GameFeeds::kAms2), true, p.ams2Port}; break;
    case F::XPlaneUdp: r = {fd.UdpPortBusy(GameFeeds::kXPlane), fd.UdpSeen(GameFeeds::kXPlane), true, p.xplanePort}; break;
    case F::DcsExport: r = {fd.DcsPortBusy(), fd.DcsSeen(), true, 49717}; break;
    case F::FlightSimConnect: r.seen = fd.FlightSimSeen(); break;
    case F::EliteStatus: r.seen = fd.EliteSeen(); break;
    default: break;
    }
    return r;
}

// What needs attention right now, from the controller's state.
std::vector<notify::Notice> CollectNotices(Controller& ctl, Integrations& in, UiState& ui, const sensors::SystemSnapshot& snap) {
    notify::Facts f;
    f.pausedAfterCrash = ctl.pause() == Controller::Pause::AfterCrash;
    f.pausedByUser = ctl.pause() == Controller::Pause::ByUser;
    const std::string maker = snap.smbios.boardMaker;
    f.asusBoard = maker.find("ASUS") != std::string::npos || maker.find("Asus") != std::string::npos;
    const auto aura = ctl.auraStatus();
    f.auraRunning = aura.running;
    f.auraConnected = aura.connected;
    f.sensorsMissing = snap.ready && !snap.lhmConnected;
    for (const auto& d : ctl.presence().inventory.devices) f.windowsProblems += d.problem ? 1 : 0;
    EnsureIntegrations(ctl, in, ui);
    for (const auto& it : in.list())
        // GameSense's Conflict means SteelSeries GG isn't answering, not a runtime to replace.
        f.integrations.push_back({it.id, it.name, it.detail, it.state == IntegrationState::Problem,
                                  it.state == IntegrationState::Conflict && it.id != "gamesense"});
    const PadInput::Snapshot pad = ctl.pad().Get();
    f.padConnected = pad.connected;
    f.padBattery = pad.state.battery;
    f.padCharging = pad.state.charging || pad.state.full;
    f.padName = pad::ModelName(pad.state.model);
    f.dualsenseWriteFailed = ctl.prefs().dualsenseController && ctl.dualsense().state() == DualSenseOutput::State::NotFound &&
                             ctl.dualsense().lastWriteError() != 0;
    // Games that are running: how long each has been up, to tell "starting" from "sends nothing".
    static std::map<std::string, uint64_t> since;
    const uint64_t now = GetTickCount64();
    std::set<std::string> running;
    for (const auto& g : ctl.games()) {
        if (!g.profile) continue;
        const std::string key = g.profile->key;
        running.insert(key);
        if (!since.count(key)) since[key] = now;
        const FeedFact ff = FeedFactOf(ctl.feeds(), ctl.prefs(), g.profile->feed);
        notify::GameFact gf;
        gf.key = key;
        gf.title = g.profile->title;
        gf.running = true;
        gf.portBusy = ff.busy;
        gf.port = ff.port;
        gf.silent = ff.needsSetup && !ff.seen && now - since[key] > 120000;
        gf.blocked = g.profile->blocked;
        f.games.push_back(gf);
    }
    for (auto it = since.begin(); it != since.end();) it = running.count(it->first) ? std::next(it) : since.erase(it);
    auto notices = notify::Collect(f);
    notify::WithGameKeys(&notices, f.games);
    return notices;
}

void OpenNotice(UiState& ui, const notify::Notice& n) {
    ui.openPending = true;
    ui.openWhere = static_cast<int>(n.where);
    ui.openArg = n.arg;
    ui.openKey = n.argKey;
}

// Takes a pending "Open" from a notice: the page and whatever it opens on (or, for a vendor
// runtime, replaces it and opens Integrations for the result). Runs before the page is drawn,
// and keeps the page's own "just arrived" resets from undoing it.
void ApplyOpen(UiState& ui, Integrations& in) {
    if (!ui.openPending) return;
    ui.openPending = false;
    using W = notify::Where;
    switch (static_cast<W>(ui.openWhere)) {
    case W::Lighting: ui.page = Page::Lighting; break;
    case W::Devices:
        ui.page = Page::Devices;
        ui.deviceDetail.clear();
        ui.devicesTab = 0;
        break;
    case W::DevicesHardware:
        ui.page = Page::Devices;
        ui.deviceDetail.clear();
        ui.devicesTab = 1;
        break;
    case W::DevicesController:
        ui.page = Page::Devices;
        ui.deviceDetail = device::kController;
        ui.devicesTab = 0;
        break;
    case W::Integrations: ui.page = Page::Integrations; ui.integrationsLoaded = false; break;
    case W::ReplaceRuntime:
        if (!in.Busy()) in.Install(ui.openArg, L"", true);  // the same as the page's "Replace vendor runtime"
        ui.page = Page::Integrations;
        break;
    case W::GamesList:
        ui.page = Page::GamesList;
        OpenGame(ui, ui.openArg, ui.openKey.empty() ? nullptr : ui.openKey.c_str());
        break;
    case W::Settings: ui.page = Page::Settings; break;
    default: return;
    }
    ui.lastPage = ui.page;  // no "just arrived" reset
    ui.devicesLanding = false;
}

unsigned SeverityColor(notify::Severity s) {
    return s == notify::Severity::Error ? kRed : s == notify::Severity::Warning ? kAmber : kAccent2;
}

// A palette color through the current style alpha, so it fades in with the list.
ImU32 Faded(unsigned rgb, float a = 1.f) { return ImGui::GetColorU32(V4(rgb, a)); }

// A notice's mark: a tinted circle with "!" for a problem, a triangle for a warning and "i" for a
// note. The shapes differ as well as the colors, so they can be told apart without color.
void SeverityMark(ImDrawList* dl, ImVec2 c, float r, notify::Severity s) {
    const unsigned col = SeverityColor(s);
    dl->AddCircleFilled(c, r, Faded(col, 0.14f), 32);
    dl->AddCircle(c, r, Faded(col, 0.45f), 32, 1.f * S());
    const ImU32 ink = Faded(col);
    switch (s) {
    case notify::Severity::Error:
        dl->AddLine(ImVec2(c.x, c.y - r * 0.48f), ImVec2(c.x, c.y + r * 0.12f), ink, 2.4f * S());
        dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.42f), 1.5f * S(), ink, 10);
        break;
    case notify::Severity::Warning:
        dl->AddTriangle(ImVec2(c.x, c.y - r * 0.52f), ImVec2(c.x + r * 0.56f, c.y + r * 0.42f),
                        ImVec2(c.x - r * 0.56f, c.y + r * 0.42f), ink, 1.6f * S());
        dl->AddLine(ImVec2(c.x, c.y - r * 0.18f), ImVec2(c.x, c.y + r * 0.1f), ink, 1.5f * S());
        dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.26f), 1.f * S(), ink, 8);
        break;
    default:
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.42f), 1.5f * S(), ink, 10);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.12f), ImVec2(c.x, c.y + r * 0.48f), ink, 2.2f * S());
        break;
    }
}

// Text that acts as a link: muted until hovered.
bool LinkText(const char* label, unsigned color = kMuted) {
    const ImVec2 p = ImGui::GetCursorScreenPos(), ts = ImGui::CalcTextSize(label, nullptr, true);
    const bool clicked = ImGui::InvisibleButton(label, ts);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(p, Faded(hovered ? kText : color), label, LabelEnd(label));
    if (hovered) dl->AddLine(ImVec2(p.x, p.y + ts.y), ImVec2(p.x + ts.x, p.y + ts.y), Faded(kText, 0.5f), 1.f);
    return clicked;
}

// A notice's action ("Open", "Open game"): a small accent-tinted pill, like the status pills.
bool NoticeAction(const char* label) {
    const ImVec2 p = ImGui::GetCursorScreenPos(), ts = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 pad(11 * S(), 4 * S()), size(ts.x + pad.x * 2, ts.y + pad.y * 2);
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Faded(kAccent, hovered ? 0.9f : 0.2f), size.y / 2);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), Faded(hovered ? 0xFFFFFF : kAccentHover), label, LabelEnd(label));
    return clicked;
}

// One notice as a tile: its mark, title, detail and action, and a cross to dismiss it.
void NoticeTile(UiState& ui, const Fonts& f, const notify::Notice& n, bool* closePopup) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float pad = 12 * S(), r = 13 * S(), cross = 22 * S();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float textX = p0.x + pad + r * 2 + 12 * S();
    const float right = p0.x + w - pad;

    // The content goes on top; the tile's background is drawn under it once its height is known.
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(textX, p0.y + pad));
    const float localX = ImGui::GetCursorPosX();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 3 * S()));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(localX + (right - cross - 6 * S() - textX));
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted(n.title.c_str());
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::PushTextWrapPos(localX + (right - textX));
    ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));
    ImGui::TextUnformatted(n.detail.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    if (n.where != notify::Where::None) {
        ImGui::Dummy(ImVec2(0, 3 * S()));
        if (NoticeAction(n.action.empty() ? "Open" : n.action.c_str())) {
            OpenNotice(ui, n);
            *closePopup = true;
        }
    }
    ImGui::EndGroup();
    ImGui::PopStyleVar();
    const ImVec2 p1(p0.x + w, ImGui::GetItemRectMax().y + pad);
    const float titleH = ImGui::GetTextLineHeight();

    // Dismiss: a cross in the top-right corner.
    ImGui::SetCursorScreenPos(ImVec2(right - cross + 4 * S(), p0.y + pad + titleH / 2 - cross / 2));
    const ImVec2 cp = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("dismiss", ImVec2(cross, cross))) ui.noticesDismissed.push_back(n.id);
    const bool crossHovered = ImGui::IsItemHovered();
    if (crossHovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Dismiss");
        dl->AddCircleFilled(ImVec2(cp.x + cross / 2, cp.y + cross / 2), cross / 2, Faded(kBorder), 20);
    }
    const ImVec2 cc(cp.x + cross / 2, cp.y + cross / 2);
    const float k = 4 * S();
    const ImU32 ink = Faded(crossHovered ? kText : kMuted);
    dl->AddLine(ImVec2(cc.x - k, cc.y - k), ImVec2(cc.x + k, cc.y + k), ink, 1.5f * S());
    dl->AddLine(ImVec2(cc.x - k, cc.y + k), ImVec2(cc.x + k, cc.y - k), ink, 1.5f * S());

    SeverityMark(dl, ImVec2(p0.x + pad + r, p0.y + pad + titleH / 2 + 2 * S()), r, n.severity);

    dl->ChannelsSetCurrent(0);
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p0, p1);
    const float round = 10 * S();
    dl->AddRectFilled(p0, p1, Faded(hovered ? kCardHover : 0x191D27), round);
    dl->AddRectFilled(p0, p1, Faded(SeverityColor(n.severity), 0.04f), round);
    dl->AddRect(p0, p1, Faded(hovered ? 0x343B4E : kBorder, 0.8f), round);
    dl->ChannelsMerge();

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, 8 * S()));
}

// The bell in the header, with a count badge; clicking it opens the list under it.
void NotificationBell(UiState& ui, const Fonts& f, const std::vector<notify::Notice>& all, float size) {
    const auto shown = notify::Visible(all, &ui.noticesDismissed);
    ImGui::PushID("bell");
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID openedAt = ImGui::GetID("opened-at"), wasOpen = ImGui::GetID("was-open");
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("bell", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    // A click outside the list closes it as the mouse goes down, so remember whether it was open
    // then: a click on the bell itself then closes the list instead of opening it again.
    if (ImGui::IsItemActivated()) st->SetBool(wasOpen, ImGui::IsPopupOpen("notices"));
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (shown.empty()) ImGui::SetTooltip("Notifications: all good");
        else ImGui::SetTooltip("%d notification%s", static_cast<int>(shown.size()), shown.size() == 1 ? "" : "s");
    }
    if (clicked && !st->GetBool(wasOpen)) {
        ImGui::OpenPopup("notices");
        st->SetFloat(openedAt, static_cast<float>(ImGui::GetTime()));
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool open = ImGui::IsPopupOpen("notices");
    const bool active = ImGui::IsItemActive();
    const ImVec2 b(a.x + size, a.y + size);
    const float rounding = 12 * S();
    dl->AddRectFilled(ImVec2(a.x, a.y + 2 * S()), ImVec2(b.x, b.y + 2 * S()), Hex(0x000000, 35), rounding);
    dl->AddRectFilled(a, b, Hex(hovered || open ? kCardHover : kTrack), rounding);
    if (open || active) dl->AddRectFilled(a, b, Hex(kAccent, active ? 50 : 32), rounding);
    dl->AddRect(a, b, Hex(open || hovered ? kAccentHover : kBorder, open ? 220 : hovered ? 160 : 210),
                rounding, 0, S());
    // A light outline keeps the bell consistent with the other header icons.
    const ImVec2 c(a.x + size / 2, a.y + size / 2);
    const float r = size * 0.20f, stroke = 1.7f * S();
    const ImU32 ink = Hex(open ? kAccentHover : hovered || !shown.empty() ? kText : kMuted);
    dl->AddLine(ImVec2(c.x, c.y - r * 1.25f), ImVec2(c.x, c.y - r), ink, stroke);
    dl->PathLineTo(ImVec2(c.x - r * 1.1f, c.y + r * 0.65f));
    dl->PathBezierCubicCurveTo(ImVec2(c.x - r * 0.75f, c.y + r * 0.35f),
                              ImVec2(c.x - r * 0.78f, c.y + r * 0.05f), ImVec2(c.x - r * 0.78f, c.y - r * 0.20f));
    dl->PathBezierCubicCurveTo(ImVec2(c.x - r * 0.78f, c.y - r * 1.25f),
                              ImVec2(c.x + r * 0.78f, c.y - r * 1.25f), ImVec2(c.x + r * 0.78f, c.y - r * 0.20f));
    dl->PathBezierCubicCurveTo(ImVec2(c.x + r * 0.78f, c.y + r * 0.05f),
                              ImVec2(c.x + r * 0.75f, c.y + r * 0.35f), ImVec2(c.x + r * 1.1f, c.y + r * 0.65f));
    dl->PathStroke(ink, ImDrawFlags_Closed, stroke);
    dl->PathArcTo(ImVec2(c.x, c.y + r * 0.82f), r * 0.28f, 0, 3.1415927f, 12);
    dl->PathStroke(ink, ImDrawFlags_None, stroke);
    if (!shown.empty()) {
        // The count in a pill colored by the most serious notice, cut out of the bell's edge.
        const std::string count = notify::BadgeText(shown.size());
        ImGui::PushFont(f.caption);
        const ImVec2 ts = ImGui::CalcTextSize(count.c_str());
        const float bh = 16 * S(), bw = std::max(bh, ts.x + 8 * S());
        const ImVec2 b0(b.x - bw + S(), a.y - 2 * S()), b1(b0.x + bw, b0.y + bh);
        const float ring = 2 * S();
        dl->AddRectFilled(ImVec2(b0.x - ring, b0.y - ring), ImVec2(b1.x + ring, b1.y + ring), Hex(kBg), bh / 2 + ring);
        dl->AddRectFilled(b0, b1, Hex(SeverityColor(notify::Worst(shown))), bh / 2);
        dl->AddText(ImVec2(b0.x + (bw - ts.x) / 2, b0.y + (bh - ts.y) / 2), Hex(0x10131A), count.c_str());
        ImGui::PopFont();
    }

    // The list, right-aligned under the bell; it fades and slides in as it opens.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = std::min(420 * S(), vp->WorkSize.x - 40 * S());
    const float t = std::clamp((static_cast<float>(ImGui::GetTime()) - st->GetFloat(openedAt)) / 0.14f, 0.f, 1.f);
    const float ease = 1.f - (1.f - t) * (1.f - t);
    const float top = a.y + size + 8 * S();
    ImGui::SetNextWindowPos(ImVec2(a.x + size - w, top + (1.f - ease) * 6 * S()));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, V4(kCard));
    ImGui::PushStyleColor(ImGuiCol_Border, V4(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ease);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14 * S());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * S(), 14 * S()));
    if (ImGui::BeginPopup("notices")) {
        bool close = false;
        // Heading: the title and what the list holds, with "Dismiss all" across from them.
        ImGui::Indent(2 * S());
        const float lineEnd = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;  // for right-aligned links
        ImGui::PushFont(f.bold);
        ImGui::TextUnformatted("Notifications");
        ImGui::PopFont();
        if (shown.size() > 1) {
            ImGui::SameLine(lineEnd - ImGui::CalcTextSize("Dismiss all").x);
            if (LinkText("Dismiss all"))
                for (const auto& n : shown) ui.noticesDismissed.push_back(n.id);
        }
        if (!shown.empty()) {
            ImGui::PushFont(f.caption);
            ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));
            ImGui::TextUnformatted(notify::Summary(shown).c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        ImGui::Unindent(2 * S());
        ImGui::Dummy(ImVec2(0, 4 * S()));

        if (shown.empty()) {
            // All clear: a check in a green circle, and one line under it.
            const float cw = ImGui::GetContentRegionAvail().x, cr = 18 * S();
            ImGui::Dummy(ImVec2(0, 10 * S()));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 cc(p.x + cw / 2, p.y + cr);
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            pdl->AddCircleFilled(cc, cr, Faded(kGreen, 0.14f), 32);
            pdl->AddCircle(cc, cr, Faded(kGreen, 0.45f), 32, 1.f * S());
            const ImVec2 check[] = {ImVec2(cc.x - cr * 0.38f, cc.y + cr * 0.02f), ImVec2(cc.x - cr * 0.1f, cc.y + cr * 0.3f),
                                    ImVec2(cc.x + cr * 0.4f, cc.y - cr * 0.28f)};
            pdl->AddPolyline(check, 3, Faded(kGreen), ImDrawFlags_None, 2.4f * S());
            ImGui::Dummy(ImVec2(cw, cr * 2 + 10 * S()));
            auto centered = [&](const char* text) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cw - ImGui::CalcTextSize(text).x) / 2);
                ImGui::TextUnformatted(text);
            };
            // With some dismissed, "all good" would claim too much.
            const bool caughtUp = !ui.noticesDismissed.empty();
            ImGui::PushFont(f.bold);
            centered(caughtUp ? "All caught up" : "All good");
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));
            centered(caughtUp ? "Nothing new since you dismissed the rest." : "Nothing needs your attention right now.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 10 * S()));
        } else {
            // The notices scroll under the heading when they don't all fit.
            const float bottom = vp->WorkPos.y + vp->WorkSize.y - 24 * S();
            const float maxH = std::max(140 * S(), std::min(460 * S(), bottom - ImGui::GetCursorScreenPos().y - 50 * S()));
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxH));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(kCard, 0.f));
            ImGui::BeginChild("notice-list", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
            int index = 0;
            for (const auto& n : shown) {
                ImGui::PushID(index++);
                NoticeTile(ui, f, n, &close);
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // Dismissed notices that still apply can be brought back.
        if (!ui.noticesDismissed.empty()) {
            ImGui::Dummy(ImVec2(0, 2 * S()));
            ImGui::Indent(2 * S());
            ImGui::PushFont(f.caption);
            const size_t hidden = ui.noticesDismissed.size();
            ImGui::PushStyleColor(ImGuiCol_Text, V4(kMuted));
            ImGui::Text("%d dismissed", static_cast<int>(hidden));
            ImGui::PopStyleColor();
            ImGui::SameLine(lineEnd - ImGui::CalcTextSize("Show again").x);
            if (LinkText("Show again")) ui.noticesDismissed.clear();
            ImGui::PopFont();
            ImGui::Unindent(2 * S());
        }
        if (close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::PopID();  // the popup shares the bell's ID scope, so OpenPopup and BeginPopup agree
}

void DrawUi(HWND hwnd, Controller& ctl, Integrations& integrations, UiState& ui, const Fonts& f) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("LumaBridge", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    integrations.SetOwner(hwnd);  // administrator prompts open in front of LumaBridge, from any page
    SampleHistory(ctl);           // the dashboard's graphs keep going on every page
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
    ApplyOpen(ui, integrations);  // "Open" (or an action) on a notification
    const std::vector<notify::Notice> notices = CollectNotices(ctl, integrations, ui, ctl.monitor().Snapshot());
    // Icon-only below 760: the sidebar's full width plus the main content couldn't both fit.
    // Its width eases towards that target instead of jumping; the content (labels or icons
    // only) switches partway through, once there's room either way.
    const bool sidebarTarget = ui.sidebarCollapsed || ImGui::GetContentRegionAvail().x < 760 * S();
    ImGuiStorage* sidebarSt = ImGui::GetStateStorage();
    const ImGuiID sidebarTid = ImGui::GetID("##sidebar-collapse-t");
    float sidebarT = sidebarSt->GetFloat(sidebarTid, sidebarTarget ? 0.f : 1.f);
    sidebarT += ((sidebarTarget ? 0.f : 1.f) - sidebarT) * std::min(1.f, ImGui::GetIO().DeltaTime * 12.f);
    sidebarSt->SetFloat(sidebarTid, sidebarT);
    const float sidebarW = 60 * S() + (210 * S() - 60 * S()) * sidebarT;
    const bool sidebarCollapsed = sidebarT < 0.5f;
    Sidebar(ctl, ui, f, sidebarW, sidebarCollapsed);
    ImGui::SameLine(0, 0);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * S(), 22 * S()));
    ImGui::BeginChild("main", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);

    // Header: page title + global mode switch.
    const char* titles[] = {"Dashboard", "My setup", "Lighting", "Games List", "Devices", "Integrations", "Settings"};  // Page order
    const char* subtitles[] = {"Your system and lighting at a glance",
                               "Your PC and desk in 3D, live",
                               "Auto follows your games; Manual is your own look",
                               "Every game on this PC, and how it lights up",
                               "Lighting controls and live readings",
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
        g_fonts = &f;
        int mode = ctl.prefs().mode == Mode::Manual ? 1 : 0;
        const float w = 250 * S(), help = 20 * S(), gap = 10 * S(), bell = 38 * S();
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - w - gap - help - gap - bell);
        const float y = ImGui::GetCursorPosY();
        NotificationBell(ui, f, notices, bell);
        ImGui::SameLine(0, gap);
        ImGui::SetCursorPosY(y + (38 * S() - help) / 2);
        HelpBadge("modes-help", help, ExplainModes);
        ImGui::SameLine(0, gap);
        ImGui::SetCursorPosY(y);
        if (ModeSwitch(&mode, w)) {
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
    if (ui.page == Page::Devices && ui.lastPage != Page::Devices) {
        ui.deviceDetail.clear();
        ui.devicesLanding = true;
    }
    if (ui.page != Page::Devices && ctl.fanTest()) ctl.SetFanTest(false);  // the test is on the fans' page
    ui.lastPage = ui.page;

    switch (ui.page) {
    case Page::Lighting:
        if (ctl.prefs().mode == Mode::Auto) AutoPage(ctl, ui, f);
        else ManualPage(ctl, ui, f);
        break;
    case Page::Devices: DevicesPage(ctl, integrations, ui, f); break;
    case Page::Dashboard: DashboardPage(hwnd, ctl, integrations, ui, f); break;
    case Page::MySetup: MySetupPage(ctl, ui, f); break;
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
