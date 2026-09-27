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
    // The app's icon as a texture (an ImTextureID: the D3D11 shader resource view), 0 if it
    // couldn't be loaded.
    unsigned long long logo = 0;
};

// Sidebar order.
enum class Page { Dashboard, Lighting, GamesList, Devices, Integrations, Settings };

struct UiState {
    Page page = Page::Dashboard;
    Page lastPage = Page::Dashboard;
    bool dashEdit = false;  // dashboard "Customize" mode
    char hex[16] = "";
    bool hexEditing = false;
    std::string hexSlot;  // which color editor the hex field being typed in belongs to
    int colorSlot = 0;    // Lighting > Manual: which of the two colors is being edited
    std::string lightTarget;  // Lighting > Manual: the device being edited ("" = all devices)
    std::string dragItem;     // Lighting: the item being dragged on the "Your setup" canvas
    // Games List: the game whose page is open ("" = the list), and its built-in profile key.
    std::string gameDetail, gameDetailProfile;
    char gameFilter[64] = "";
    // Devices: the device whose page is open ("" = the list): "aura:<Aura name>" or a
    // device::k* id (memory, mouse, keyboard).
    std::string deviceDetail;
    // Built-in game feed setup state (Integrations page), re-checked every couple of seconds.
    unsigned long long feedCheckAt = 0;
    bool cs2Installed = false, rlIniFound = false, rlEnabled = false;
    std::wstring cs2Dir, rlDir;
    std::string feedMessage, feedMessageId;  // result of the last direct (non-elevated) write
    bool integrationsLoaded = false;
    bool autostart = false;
    bool autostartLoaded = false;
    double splashStart = -1;  // when the loading screen appeared (ImGui time), -1 before the first frame
};

// Applies the LumaBridge theme at the given DPI scale (call again when DPI changes).
void ApplyTheme(float scale);

void DrawUi(HWND hwnd, Controller& ctl, Integrations& integrations, UiState& ui, const Fonts& fonts);

}  // namespace luma::app
