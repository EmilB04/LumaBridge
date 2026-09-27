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
enum class Page { Dashboard, Lighting, GamesList, Devices, Integrations, Settings };

struct UiState {
    Page page = Page::Dashboard;
    Page lastPage = Page::Dashboard;
    bool dashEdit = false;  // dashboard "Customize" mode
    char hex[16] = "";
    bool hexEditing = false;
    char gameFilter[64] = "";
    // Built-in game feed setup state (Integrations page), re-checked every couple of seconds.
    unsigned long long feedCheckAt = 0;
    bool cs2Installed = false, rlIniFound = false, rlEnabled = false;
    std::wstring cs2Dir, rlDir;
    std::string feedMessage, feedMessageId;  // result of the last direct (non-elevated) write
    bool integrationsLoaded = false;
    bool autostart = false;
    bool autostartLoaded = false;
};

// Applies the LumaBridge theme at the given DPI scale (call again when DPI changes).
void ApplyTheme(float scale);

void DrawUi(HWND hwnd, Controller& ctl, Integrations& integrations, UiState& ui, const Fonts& fonts);

}  // namespace luma::app
