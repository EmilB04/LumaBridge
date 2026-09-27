#include "ui.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <functional>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "controller.h"
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

float g_scale = 1.f;  // DPI scale, set by ApplyTheme
float S() { return g_scale; }

Rgb FromV4(const float* f) {
    auto b = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    return Rgb{b(f[0]), b(f[1]), b(f[2])};
}

// Color actually visible right now (animates breathing / strobe for the preview).
Rgb PreviewColor(const Controller::Output& o) {
    const double t = ImGui::GetTime();
    switch (o.kind) {
    case Controller::Output::Kind::Breathing: {
        double k = 0.5 + 0.5 * std::cos(t * o.hz * 2.0 * 3.14159265358979);
        return Scale(o.color, k);
    }
    case Controller::Output::Kind::Strobe:
        return std::fmod(t * o.hz, 1.0) < 0.5 ? o.color : Rgb{};
    case Controller::Output::Kind::ArmouryCrate:
        return Rgb{60, 64, 76};
    default:
        return o.color;
    }
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
    const auto& out = ctl.output();
    BeginCard("now");
    CardTitle(f, "Dynamic lighting");
    Muted("Games drive your lights. When several are running, the one that changed color most recently wins.");
    ImGui::Dummy(ImVec2(0, 8 * S()));

    const auto& sources = ctl.sources();
    if (sources.empty()) {
        Pill("Waiting for a game", kMuted);
        ImGui::Dummy(ImVec2(0, 4 * S()));
        Muted("Start a game with Logitech, Razer, SteelSeries, Corsair or Alienware lighting. "
              "Make sure it is set up on the Games page.");
    } else {
        const uint64_t now = GetTickCount64();
        for (const auto& s : sources) {
            ImGui::PushID(static_cast<int>(s.pid) ^ static_cast<int>(std::hash<std::string>{}(s.sdk)));
            Swatch("c", s.color, 28 * S(), out.label == s.game + " - " + s.sdk);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::PushFont(f.bold);
            ImGui::TextUnformatted(s.game.c_str());
            ImGui::PopFont();
            Muted("%s  -  %s  -  updated %llus ago", s.sdk.c_str(), ToHex(s.color).c_str(),
                  static_cast<unsigned long long>((now - s.lastChange) / 1000));
            ImGui::EndGroup();
            ImGui::PopID();
        }
    }
    EndCard();

    BeginCard("idle");
    CardTitle(f, "When no game is running");
    int idle = ctl.prefs().idle == IdleBehavior::ManualColor ? 1 : 0;
    const char* labels[] = {"Armoury Crate effects", "My manual color"};
    if (Segmented("idle", &idle, labels, 2, ImGui::GetContentRegionAvail().x)) {
        ctl.prefs().idle = idle ? IdleBehavior::ManualColor : IdleBehavior::ArmouryCrate;
        ctl.Changed();
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    Muted("%s", idle ? "Your manual color (and effect) shows between games."
                     : "Lighting is handed back to Armoury Crate between games.");
    EndCard();

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
        int effect = static_cast<int>(p.effect);
        const char* effects[] = {"Static", "Breathing", "Strobe"};
        if (Segmented("effect", &effect, effects, 3, colW)) {
            p.effect = static_cast<ManualEffect>(effect);
            ctl.Changed();
        }
        if (p.effect != ManualEffect::Static) {
            ImGui::Dummy(ImVec2(0, 4 * S()));
            if (LabeledSlider("Speed", &p.speedHz, 0.1f, p.effect == ManualEffect::Strobe ? 10.f : 3.f,
                              "%.1f per second"))
                ctl.Changed();
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

    BrightnessCard(ctl, f);
}

void DevicesPage(Controller& ctl, const Fonts& f) {
    static std::vector<AuraDeviceInfo> lastDevices;  // survives while Armoury Crate has control
    auto st = ctl.auraStatus();
    if (!st.devices.empty()) lastDevices = st.devices;

    BeginCard("status");
    CardTitle(f, "Aura connection");
    if (st.connected) {
        Pill("Connected", kGreen);
        ImGui::SameLine();
        Muted("%d device(s) under LumaBridge control", static_cast<int>(st.devices.size()));
    } else if (!st.running) {
        Pill("Armoury Crate has control", kMuted);
        ImGui::SameLine();
        Muted("LumaBridge takes over when a game or your manual color needs the lights.");
    } else {
        Pill("Aura service not found", kRed);
        ImGui::SameLine();
        Muted("Install Armoury Crate and make sure its lighting service is running.");
    }
    ImGui::Dummy(ImVec2(0, 4 * S()));
    if (ImGui::Button("Rescan devices")) ctl.RescanDevices();
    EndCard();

    BeginCard("devices");
    CardTitle(f, "Devices");
    if (lastDevices.empty()) {
        Muted("No devices seen yet. Switch to Manual mode once so LumaBridge can scan your Aura devices.");
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
            ImGui::TextUnformatted(name);
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
}

void GamesPage(HWND hwnd, Controller& ctl, Integrations& in, UiState& ui, const Fonts& f) {
    const auto& gs = ctl.gameSense();
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
        case IntegrationState::Problem: Pill("Needs repair", kRed); break;
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
        } else if (it.state == IntegrationState::Active) {
            if (ImGui::Button("Remove")) in.Remove(it.id);
        } else if (it.state == IntegrationState::Conflict) {
            if (ImGui::Button("Replace vendor runtime")) in.Install(it.id, L"", true);
        } else {
            if (PrimaryButton(it.id == "logitech" ? "Set up" : "Install")) in.Install(it.id);
        }
        ImGui::EndDisabled();
        EndCard();
    }
    std::string msg = in.LastMessage();
    if (!msg.empty()) Muted("%s", msg.c_str());
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
    } items[] = {{Page::Lighting, "Lighting"}, {Page::Devices, "Devices"}, {Page::Games, "Games"},
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
    ImGui::SetCursorPosY(bottom - orbR * 2 - 58 * S());
    ImVec2 p = ImGui::GetCursorScreenPos();
    Orb(ImVec2(p.x + orbR + 4 * S(), p.y + orbR + 4 * S()), orbR, PreviewColor(out));
    ImGui::Dummy(ImVec2(0, orbR * 2 + 14 * S()));
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted(ctl.prefs().mode == Mode::Auto ? "Auto" : "Manual");
    ImGui::PopFont();
    ImGui::PushFont(f.caption);
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
    const char* titles[] = {"Lighting", "Devices", "Games", "Settings"};
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

    if (ctl.auraPaused()) {
        BeginCard("paused");
        Pill("Lighting control paused", kAmber);
        ImGui::Dummy(ImVec2(0, 2 * S()));
        Muted("LumaBridge closed unexpectedly the last time it controlled your lights, so it is "
              "leaving them to Armoury Crate for now. Details are in the log folder (Settings).");
        if (PrimaryButton("Resume lighting control")) ctl.ResumeAura();
        EndCard();
    }

    // Re-check integration status whenever the Games page is opened.
    if (ui.page == Page::Games && ui.lastPage != Page::Games) ui.integrationsLoaded = false;
    ui.lastPage = ui.page;

    switch (ui.page) {
    case Page::Lighting:
        if (ctl.prefs().mode == Mode::Auto) AutoPage(ctl, f);
        else ManualPage(ctl, ui, f);
        break;
    case Page::Devices: DevicesPage(ctl, f); break;
    case Page::Games: GamesPage(hwnd, ctl, integrations, ui, f); break;
    case Page::Settings: SettingsPage(ctl, ui, f); break;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

}  // namespace luma::app
