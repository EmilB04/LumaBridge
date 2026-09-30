#include "displays.h"

#include <windows.h>

#include <string>

namespace luma::app::displays {
namespace {

std::string Utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    out.pop_back();
    return out;
}

// The monitor's own name ("DELL S2721DGF") for a display (\\.\DISPLAY1), from Windows'
// display configuration; "" if it doesn't say.
std::string FriendlyName(const wchar_t* gdiName, std::string* id) {
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr) != ERROR_SUCCESS) return {};
    for (UINT32 i = 0; i < paths; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = p[i].sourceInfo.adapterId;
        source.header.id = p[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, gdiName) != 0) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof target;
        target.header.adapterId = p[i].targetInfo.adapterId;
        target.header.id = p[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
            *id = Utf8(target.monitorDevicePath);
            return Utf8(target.monitorFriendlyDeviceName);
        }
    }
    return {};
}

BOOL CALLBACK Add(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* list = reinterpret_cast<std::vector<Display>*>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof info;
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    Display d;
    d.x = info.rcMonitor.left;
    d.y = info.rcMonitor.top;
    d.w = info.rcMonitor.right - info.rcMonitor.left;
    d.h = info.rcMonitor.bottom - info.rcMonitor.top;
    d.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof mode;
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) d.hz = static_cast<int>(mode.dmDisplayFrequency);
    if (HDC dc = CreateDCW(L"DISPLAY", info.szDevice, nullptr, nullptr)) {
        d.widthCm = static_cast<float>(GetDeviceCaps(dc, HORZSIZE)) / 10.f;
        d.heightCm = static_cast<float>(GetDeviceCaps(dc, VERTSIZE)) / 10.f;
        DeleteDC(dc);
    }
    d.name = FriendlyName(info.szDevice, &d.id);
    if (d.name.empty()) d.name = Utf8(info.szDevice);
    list->push_back(d);
    return TRUE;
}

}  // namespace

std::vector<Display> List() {
    std::vector<Display> out;
    EnumDisplayMonitors(nullptr, nullptr, Add, reinterpret_cast<LPARAM>(&out));
    return out;
}

}  // namespace luma::app::displays
