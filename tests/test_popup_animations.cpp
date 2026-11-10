// Headless ImGui frames check the actual popup and child draw lists without a GPU.
#include "popup_animations.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static unsigned Alpha(ImDrawList* list) {
    return list && !list->VtxBuffer.empty() ? (list->VtxBuffer[0].col >> IM_COL32_A_SHIFT) & 255 : 0;
}

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.f / 120;
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    io.Fonts->SetTexID(1);
    luma::app::PopupAnimations animations;
    ImDrawList *popup = nullptr, *child = nullptr, *host = nullptr;
    unsigned rawPopup = 0, rawChild = 0;
    bool childMoved = false;
    ImGuiWindow* popupWindow = nullptr;

    auto frame = [&](bool show, bool open = false, bool tooltip = false, int combo = 0) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(800, 600));
        ImGui::Begin("host");
        host = ImGui::GetWindowDrawList();
        ImGui::Button("background");
        if (open && !combo) ImGui::OpenPopup("menu");
        popup = child = nullptr;
        if (show) {
            ImGui::SetNextWindowPos(ImVec2(200, 100));
            ImGui::SetNextWindowSize(ImVec2(250, 160));
            if (ImGui::BeginPopup("menu")) {
                popup = ImGui::GetWindowDrawList();
                popupWindow = ImGui::GetCurrentWindow();
                ImGui::Selectable("Choose a zone");
                ImGui::BeginChild("scrolling", ImVec2(150, 70), ImGuiChildFlags_Borders);
                ImGui::TextUnformatted("Nested content");
                child = ImGui::GetWindowDrawList();
                ImGui::EndChild();
                ImGui::EndPopup();
            }
        }
        if (combo) {
            const char* label = combo == 1 ? "first dropdown" : "second dropdown";
            if (open) ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID(label)), ImGuiPopupFlags_None);
            if (ImGui::BeginCombo(label, "Choose")) {
                popup = ImGui::GetWindowDrawList();
                popupWindow = ImGui::GetCurrentWindow();
                ImGui::Selectable("Option one");
                ImGui::Selectable("Option two");
                ImGui::EndCombo();
            }
        }
        if (tooltip) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Hover hint");
            popup = ImGui::GetWindowDrawList();
            ImGui::EndTooltip();
        }
        ImGui::End();
        ImGui::Render();
        const unsigned rawHost = Alpha(host);
        rawPopup = Alpha(popup);
        rawChild = Alpha(child);
        const ImVec2 before = child && !child->VtxBuffer.empty() ? child->VtxBuffer[0].pos : ImVec2();
        animations.Render();
        CHECK(Alpha(host) == rawHost);
        if (child && !child->VtxBuffer.empty())
            childMoved |= child->VtxBuffer[0].pos.x != before.x || child->VtxBuffer[0].pos.y != before.y;
        const auto* data = ImGui::GetDrawData();
        int vertices = 0, indices = 0;
        for (const auto* list : data->CmdLists) { vertices += list->VtxBuffer.Size; indices += list->IdxBuffer.Size; }
        CHECK(data->CmdListsCount == data->CmdLists.Size);
        CHECK(data->TotalVtxCount == vertices && data->TotalIdxCount == indices);
        return data->CmdListsCount;
    };

    frame(true, true);
    frame(true);
    CHECK(popup && Alpha(popup) > 0 && Alpha(popup) < rawPopup);
    CHECK(child && Alpha(child) > 0 && Alpha(child) < rawChild);
    const unsigned once = Alpha(popup);
    animations.Render();
    CHECK(Alpha(popup) == once);
    for (int i = 0; i < 30; ++i) frame(true);
    CHECK(Alpha(popup) == rawPopup && Alpha(child) == rawChild);
    CHECK(!childMoved);  // fading never displaces hit targets or clips child content
    io.DisplaySize.x = 900;
    frame(true);
    CHECK(Alpha(popup) == rawPopup);

    ImGui::ClosePopupToLevel(0, true);
    const int closingCount = frame(false);
    CHECK(closingCount > 1);
    CHECK(GImGui->OpenPopupStack.empty());
    CHECK(popupWindow && !popupWindow->Active);
    frame(true, true);  // reopen before the closing animation finishes
    CHECK(popup && Alpha(popup) > rawPopup / 2);
    for (int i = 0; i < 30; ++i) frame(true);
    ImGui::ClosePopupToLevel(0, true);
    frame(false);
    for (int i = 0; i < 20; ++i) frame(false);
    CHECK(ImGui::GetDrawData()->CmdListsCount == 1);

    frame(false, true, false, 1);
    for (int i = 0; i < 30; ++i) frame(false, false, false, 1);
    CHECK(popup && Alpha(popup) == rawPopup);
    const ImGuiID firstWindow = popupWindow->ID, firstPopup = popupWindow->PopupId;
    ImGui::ClosePopupToLevel(0, true);
    frame(false, true, false, 2);
    frame(false, false, false, 2);
    CHECK(popup && Alpha(popup) < rawPopup);
    CHECK(popupWindow->ID == firstWindow && popupWindow->PopupId != firstPopup);
    ImGui::ClosePopupToLevel(0, true);
    for (int i = 0; i < 20; ++i) frame(false);

    frame(false, false, true);
    frame(false, false, true);
    CHECK(popup && Alpha(popup) < rawPopup);
    for (int i = 0; i < 30; ++i) frame(false, false, true);
    CHECK(Alpha(popup) == rawPopup);
    CHECK(frame(false) > 1);
    animations.Reset();  // simulates replacing the font atlas on a DPI change
    CHECK(frame(false) == 1);
    animations.Reset();
    ImGui::DestroyContext();
    if (failures) return EXIT_FAILURE;
    std::puts("popup animation tests passed");
    return EXIT_SUCCESS;
}
