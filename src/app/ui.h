// The LumaBridge window, drawn with Dear ImGui.
#pragma once

#include <windows.h>

#include <string>

struct ImFont;

namespace luma::app {

class Controller;
class Integrations;

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImFont* title = nullptr;
    ImFont* caption = nullptr;  // not "small": rpcndr.h #defines small
};

// Sidebar order.
enum class Page { Lighting, GamesList, Devices, Integrations, Settings };

struct UiState {
    Page page = Page::Lighting;
    Page lastPage = Page::Lighting;
    char hex[16] = "";
    bool hexEditing = false;
    char gameFilter[64] = "";
    bool integrationsLoaded = false;
    bool autostart = false;
    bool autostartLoaded = false;
};

// Applies the LumaBridge theme at the given DPI scale (call again when DPI changes).
void ApplyTheme(float scale);

void DrawUi(HWND hwnd, Controller& ctl, Integrations& integrations, UiState& ui, const Fonts& fonts);

}  // namespace luma::app
