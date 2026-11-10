#include "popup_animations.h"
#include "popup_transition.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

namespace luma::app {
namespace {

struct DeleteDrawList {
    void operator()(ImDrawList* list) const { IM_DELETE(list); }
};
using Drawing = std::unique_ptr<ImDrawList, DeleteDrawList>;
using Key = uint64_t;

void Fade(ImDrawList& list, float alpha) {
    if (alpha >= 1.f) return;
    for (ImDrawVert& v : list.VtxBuffer) {
        const unsigned a = static_cast<unsigned>(((v.col >> IM_COL32_A_SHIFT) & 255) * alpha + 0.5f);
        v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}

bool CanRetain(const ImDrawList& list) {
    // A callback may refer to temporary state that no longer exists after closing.
    for (const ImDrawCmd& cmd : list.CmdBuffer)
        if (cmd.UserCallback) return false;
    return true;
}

}  // namespace

struct PopupAnimations::Impl {
    struct Entry {
        PopupTransition fade;
        std::vector<Drawing> saved;
        bool visible = false, tooltip = false;
        unsigned order = 0;
    };
    std::unordered_map<Key, Entry> entries;
    std::vector<Drawing> closing;
    ImGuiContext* context = nullptr;
    int frame = -1;
    ImVec2 size{};
};

PopupAnimations::PopupAnimations() : impl_(std::make_unique<Impl>()) {}
PopupAnimations::~PopupAnimations() = default;

void PopupAnimations::Reset() { impl_ = std::make_unique<Impl>(); }

void PopupAnimations::Render() {
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context) return;
    ImDrawData* data = ImGui::GetDrawData();
    if (!data || !data->Valid) return;
    if (impl_->context != context) Reset();
    Impl& state = *impl_;
    if (state.frame == context->FrameCount) return;
    // Discard drawings with old viewport bounds without restarting an open menu's fade.
    if (state.size.x != data->DisplaySize.x || state.size.y != data->DisplaySize.y)
        for (auto& pair : state.entries) pair.second.saved.clear();
    state.context = context;
    state.frame = context->FrameCount;
    state.size = data->DisplaySize;
    state.closing.clear();
    for (auto& pair : state.entries) pair.second.visible = false;

    // RootWindow includes children such as a scrolling notification list. Combo windows
    // are reused by ImGui, so PopupId distinguishes the controls that opened them.
    std::unordered_map<ImDrawList*, Key> owners;
    for (ImGuiWindow* window : context->Windows) {
        ImGuiWindow* root = window->RootWindow;
        if (!window->Active || window->Hidden || !root ||
            !(root->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip))) continue;
        const bool tooltip = (root->Flags & ImGuiWindowFlags_Tooltip) != 0;
        ImGuiID owner = root->PopupId;
        if (tooltip) {
            const ImGuiID hovered[] = {context->HoveredId, context->HoveredWindow ? context->HoveredWindow->ID : 0};
            owner = ImHashData(hovered, sizeof(hovered));
        }
        const Key key = (static_cast<Key>(root->ID) << 32) | owner;
        owners.emplace(window->DrawList, key);
        state.entries[key].tooltip = tooltip;
    }

    std::vector<ImDrawList*> original;
    std::vector<Key> order;
    bool hasTooltip = false;
    size_t firstPopup = static_cast<size_t>(data->CmdListsCount);
    for (ImDrawList* list : data->CmdLists) {
        original.push_back(list);
        const auto owner = owners.find(list);
        if (owner == owners.end()) continue;
        firstPopup = std::min(firstPopup, original.size() - 1);
        auto& entry = state.entries[owner->second];
        if (!entry.visible) {
            entry.visible = true;
            entry.saved.clear();
            entry.order = static_cast<unsigned>(order.size());
            order.push_back(owner->second);
        }
        hasTooltip |= entry.tooltip;
        if (CanRetain(*list)) entry.saved.emplace_back(list->CloneOutput());
    }
    for (Key key : order) {
        auto& entry = state.entries[key];
        const float alpha = entry.fade.Advance(true, context->IO.DeltaTime);
        for (ImDrawList* list : original) {
            const auto owner = owners.find(list);
            if (owner != owners.end() && owner->second == key) Fade(*list, alpha);
        }
    }

    std::vector<Impl::Entry*> fading;
    for (auto it = state.entries.begin(); it != state.entries.end();) {
        auto& entry = it->second;
        if (entry.visible) { ++it; continue; }
        const float alpha = entry.fade.Advance(false, context->IO.DeltaTime);
        // A replacement tooltip takes over immediately, avoiding overlapping hover texts.
        if (alpha <= 0 || (entry.tooltip && hasTooltip)) { it = state.entries.erase(it); continue; }
        fading.push_back(&entry);
        ++it;
    }
    std::sort(fading.begin(), fading.end(), [](const auto* a, const auto* b) { return a->order < b->order; });
    for (const auto* entry : fading) {
        const float alpha = entry->fade.Alpha();
        for (const auto& saved : entry->saved) {
            Drawing copy(saved->CloneOutput());
            Fade(*copy, alpha);
            state.closing.push_back(std::move(copy));
        }
    }

    // Insert fading drawings below any newly opened popup, without adding an ImGui
    // window that could steal focus, trap a click, or keep a closed menu alive.
    data->CmdLists.resize(0);
    data->TotalVtxCount = data->TotalIdxCount = 0;
    auto append = [&](ImDrawList* list) {
        data->CmdLists.push_back(list);
        data->TotalVtxCount += list->VtxBuffer.Size;
        data->TotalIdxCount += list->IdxBuffer.Size;
    };
    for (size_t i = 0; i <= original.size(); ++i) {
        if (i == firstPopup) for (const auto& list : state.closing) append(list.get());
        if (i < original.size()) append(original[i]);
    }
    data->CmdListsCount = data->CmdLists.Size;
}

}  // namespace luma::app
