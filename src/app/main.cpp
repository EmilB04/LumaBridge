// LumaBridge.exe: tray app + window. Owns Aura, hosts GameSense, receives game DLL frames.
//
//   LumaBridge.exe              open the window
//   LumaBridge.exe --minimized  start in the tray (used by "Start with Windows")
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <cmath>
#include <cwchar>
#include <iterator>

#include "controller.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "integrations.h"
#include "ipc.h"
#include "ui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace luma;
using namespace luma::app;

namespace {

constexpr wchar_t kMainClass[] = L"LumaBridgeMainWnd";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_SHOW_WINDOW = WM_APP + 2;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickMs = 50;
enum : UINT { kMenuOpen = 1, kMenuAuto, kMenuManual, kMenuExit };

Controller g_ctl;
Integrations g_integrations;
UiState g_ui;
Fonts g_fonts;

HWND g_main = nullptr;
HWND g_ipc = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbarCreated = 0;
HICON g_trayIcon = nullptr;
Rgb g_trayColor{1, 2, 3};

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resizeW = 0, g_resizeH = 0;
float g_dpiScale = 1.f;
bool g_rebuildFonts = false;

// ---- Direct3D ------------------------------------------------------------------------

void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void CleanupRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU driver: software rasterizer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDevice() {
    CleanupRenderTarget();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swapChain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

// ---- Fonts ---------------------------------------------------------------------------

void LoadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t winDir[MAX_PATH];
    GetWindowsDirectoryW(winDir, MAX_PATH);
    char fonts[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, winDir, -1, fonts, sizeof fonts, nullptr, nullptr);
    std::string dir = std::string(fonts) + "\\Fonts\\";
    auto load = [&](const char* file, float size) -> ImFont* {
        std::string path = dir + file;
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
        return io.Fonts->AddFontFromFileTTF(path.c_str(), std::round(size * scale));
    };
    g_fonts.regular = load("segoeui.ttf", 16.f);
    if (!g_fonts.regular) {
        ImFontConfig cfg;
        cfg.SizePixels = std::round(13.f * scale);
        g_fonts.regular = io.Fonts->AddFontDefault(&cfg);
    }
    g_fonts.bold = load("segoeuib.ttf", 16.f);
    g_fonts.title = load("segoeuib.ttf", 26.f);
    g_fonts.caption = load("segoeui.ttf", 13.5f);
    if (!g_fonts.bold) g_fonts.bold = g_fonts.regular;
    if (!g_fonts.title) g_fonts.title = g_fonts.regular;
    if (!g_fonts.caption) g_fonts.caption = g_fonts.regular;
    io.FontDefault = g_fonts.regular;
}

// ---- Tray ----------------------------------------------------------------------------

// A small glowing dot in the current lighting color.
HICON MakeOrbIcon(Rgb c) {
    const int n = GetSystemMetrics(SM_CXSMICON);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = n;
    bi.bV5Height = -n;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    auto* px = static_cast<uint32_t*>(bits);
    const float r = n / 2.f - 0.5f;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float d = std::hypot(x - r, y - r) / r;  // 0 center .. 1 edge
            float a = std::clamp((1.f - d) * 3.f, 0.f, 1.f);  // soft edge
            float shade = 1.f - 0.25f * d;
            auto ch = [&](uint8_t v) { return static_cast<uint32_t>(v * shade * a); };  // premultiplied
            px[y * n + x] = (static_cast<uint32_t>(a * 255) << 24) | (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
        }
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void UpdateTray(bool add) {
    const auto& out = g_ctl.output();
    Rgb c = out.kind == Controller::Output::Kind::ArmouryCrate ? Rgb{120, 124, 140} : out.color;
    if (c.IsBlack()) c = Rgb{40, 40, 48};
    bool iconChanged = add || c != g_trayColor;
    if (iconChanged) {
        HICON icon = MakeOrbIcon(c);
        if (icon) {
            if (g_trayIcon) DestroyIcon(g_trayIcon);
            g_trayIcon = icon;
            g_trayColor = c;
        }
    }
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_trayIcon ? g_trayIcon : LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    wchar_t tip[128];
    MultiByteToWideChar(CP_UTF8, 0, ("LumaBridge - " + out.label).c_str(), -1, tip, static_cast<int>(std::size(tip)));
    wcsncpy(g_nid.szTip, tip, std::size(g_nid.szTip) - 1);
    if (add) Shell_NotifyIconW(NIM_ADD, &g_nid);
    else if (iconChanged) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void ShowMain() {
    ShowWindow(g_main, IsIconic(g_main) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_main);
}

void TrayMenu() {
    HMENU m = CreatePopupMenu();
    const bool manual = g_ctl.prefs().mode == Mode::Manual;
    AppendMenuW(m, MF_STRING, kMenuOpen, L"Open LumaBridge");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (manual ? 0 : MF_CHECKED), kMenuAuto, L"Auto (games)");
    AppendMenuW(m, MF_STRING | (manual ? MF_CHECKED : 0), kMenuManual, L"Manual color");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuExit, L"Exit");
    SetMenuDefaultItem(m, kMenuOpen, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case kMenuOpen: ShowMain(); break;
    case kMenuAuto: g_ctl.prefs().mode = Mode::Auto; g_ctl.Changed(); break;
    case kMenuManual: g_ctl.prefs().mode = Mode::Manual; g_ctl.Changed(); break;
    case kMenuExit: PostQuitMessage(0); break;
    }
}

// ---- Windows -------------------------------------------------------------------------

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if (msg == g_taskbarCreated && g_taskbarCreated) {  // Explorer restarted
        UpdateTray(true);
        return 0;
    }
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_resizeW = LOWORD(lp);
            g_resizeH = HIWORD(lp);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = static_cast<LONG>(860 * g_dpiScale);
        mm->ptMinTrackSize.y = static_cast<LONG>(600 * g_dpiScale);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpiScale = HIWORD(wp) / 96.f;
        g_rebuildFonts = true;
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:  // close = hide to tray; Exit is in the tray menu
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) ShowMain();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) TrayMenu();
        return 0;
    case WM_SHOW_WINDOW:
        ShowMain();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Message-only window: IPC from game DLLs, and the controller tick (runs even when hidden).
LRESULT CALLBACK IpcProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        ipc::Frame f;
        if (cds && cds->dwData == ipc::kCopyDataTag && ipc::ParseFrame(cds->lpData, cds->cbData, &f)) {
            g_ctl.OnIpc(f);
            return TRUE;
        }
        return FALSE;
    }
    if (msg == WM_TIMER && wp == kTickTimer) {
        g_ctl.Tick();
        UpdateTray(false);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\LumaBridgeApp");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kMainClass, nullptr)) PostMessageW(other, WM_SHOW_WINDOW, 0, 0);
        return 0;
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // file dialogs
    g_ctl.Init();
    const bool startHidden = wcsstr(cmdLine, L"--minimized") && g_ctl.prefs().startMinimized;

    // IPC window first, so game DLLs start routing to us as early as possible.
    WNDCLASSEXW ic{};
    ic.cbSize = sizeof ic;
    ic.lpfnWndProc = IpcProc;
    ic.hInstance = inst;
    ic.lpszClassName = ipc::kAppWindowClass;
    RegisterClassExW(&ic);
    g_ipc = CreateWindowExW(0, ipc::kAppWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    ChangeWindowMessageFilterEx(g_ipc, WM_COPYDATA, MSGFLT_ALLOW, nullptr);  // games running elevated
    SetTimer(g_ipc, kTickTimer, kTickMs, nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    POINT origin{0, 0};
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    g_dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
    const int w = static_cast<int>(980 * g_dpiScale), h = static_cast<int>(700 * g_dpiScale);
    g_main = CreateWindowExW(0, kMainClass, L"LumaBridge", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                             nullptr, nullptr, inst, nullptr);
    g_dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(g_main);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_main, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (!CreateDevice(g_main)) {
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"LumaBridge", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // layout is fixed; nothing to persist
    ApplyTheme(g_dpiScale);
    LoadFonts(g_dpiScale);
    ImGui_ImplWin32_Init(g_main);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_ctl.Tick();
    UpdateTray(true);
    if (!startHidden) {
        ShowWindow(g_main, SW_SHOWDEFAULT);
        UpdateWindow(g_main);
    }

    bool running = true;
    while (running) {
        if (!IsWindowVisible(g_main) || IsIconic(g_main)) {
            // In the tray: no rendering, just messages (IPC + tick timer keep working).
            MSG msg;
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        if (g_resizeW && g_resizeH) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }
        if (g_rebuildFonts) {
            g_rebuildFonts = false;
            ApplyTheme(g_dpiScale);
            LoadFonts(g_dpiScale);
            ImGui_ImplDX11_InvalidateDeviceObjects();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi(g_main, g_ctl, g_integrations, g_ui, g_fonts);
        ImGui::Render();
        const float clear[4] = {0.055f, 0.063f, 0.078f, 1.f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);  // vsync
    }

    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    KillTimer(g_ipc, kTickTimer);
    g_ctl.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(g_main);
    DestroyWindow(g_ipc);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
