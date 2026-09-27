// ROG Azoth per-key lighting probe: tries the per-key color command and maps the keyboard's
// LED numbers to its keys.
//
//   azoth-probe            the guided test: all keys one color, then a pattern, then the map
//
// The command tried (ASUS ROG keyboards, as OpenRGB drives them), on the same interface as
// the effects (see src/app/peripherals/azoth_protocol.h):
//   C0 81 <n> 00  then n (up to 15) x <LED number> <R> <G> <B>
// Mapping: one LED lights red at a time and you press that key; the tool reads which key it
// is from the console (a left click: nothing lit; a right click: a key that types nothing,
// like Fn). Nothing is ever saved to the keyboard (no 50 55).
// Output: the console and %LOCALAPPDATA%\LumaBridge\azoth-probe.txt, the map also in
// azoth-map.txt next to it.
#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/app/peripherals/azoth_protocol.h"

namespace {

using namespace luma::app;

FILE* g_out = nullptr;

void Say(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    printf("%s\n", buf);
    if (g_out) {
        fprintf(g_out, "%s\n", buf);
        fflush(g_out);
    }
}

HANDLE OpenAzoth(azoth::Link link) {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr)) continue;
        HANDLE q = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof a;
        bool match = HidD_GetAttributes(q, &a) && a.VendorID == azoth::kVendor && a.ProductID == azoth::Product(link);
        if (match) {
            PHIDP_PREPARSED_DATA pre = nullptr;
            HIDP_CAPS caps{};
            match = HidD_GetPreparsedData(q, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == azoth::kUsagePage && caps.OutputReportByteLength == azoth::ReportSize(link);
            if (pre) HidD_FreePreparsedData(pre);
        }
        CloseHandle(q);
        if (match)
            found = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

HANDLE g_dev = INVALID_HANDLE_VALUE;
azoth::Link g_link = azoth::Link::Wired;
int g_writeErrors = 0;

bool Write(const std::vector<uint8_t>& cmd) {
    azoth::Report r{};
    r[0] = g_link == azoth::Link::Wired ? 0x00 : 0x02;
    for (size_t i = 0; i < cmd.size() && i + 1 < r.size(); ++i) r[i + 1] = cmd[i];
    if (azoth::IsSave(r)) return false;  // never
    DWORD n = 0;
    const bool ok = WriteFile(g_dev, r.data(), static_cast<DWORD>(azoth::ReportSize(g_link)), &n, nullptr) != 0;
    if (!ok) ++g_writeErrors;
    Sleep(4);
    return ok;
}

using Rgb3 = std::array<uint8_t, 3>;

// Sets LEDs [0, count) with the per-key command, 15 per report.
void SetKeys(int count, Rgb3 (*colorOf)(int, void*), void* ctx) {
    for (int first = 0; first < count; first += 15) {
        const int n = std::min(15, count - first);
        std::vector<uint8_t> cmd{0xC0, 0x81, static_cast<uint8_t>(n), 0x00};
        for (int k = 0; k < n; ++k) {
            const Rgb3 c = colorOf(first + k, ctx);
            cmd.insert(cmd.end(), {static_cast<uint8_t>(first + k), c[0], c[1], c[2]});
        }
        Write(cmd);
    }
}

constexpr int kLeds = 144;  // more than a 75 % board has; the extra numbers light nothing

Rgb3 Hue(float h) {  // 0..1
    const float r = std::max(0.f, std::min(1.f, std::abs(h * 6 - 3) - 1));
    const float g = std::max(0.f, std::min(1.f, 2 - std::abs(h * 6 - 2)));
    const float b = std::max(0.f, std::min(1.f, 2 - std::abs(h * 6 - 4)));
    return {static_cast<uint8_t>(r * 255), static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255)};
}

std::string Ask(const char* q) {
    printf("%s ", q);
    fflush(stdout);
    char line[64] = {};
    if (!fgets(line, sizeof line, stdin)) return "";
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (g_out) fprintf(g_out, "%s %s\n", q, s.c_str());
    return s;
}

// Waits for a key press or a click. Returns the key's name ("" for a click), `none`: left
// click (nothing lit), `silent`: right click (a key that types nothing).
std::string WaitKey(bool* none, bool* silent, unsigned* vk, unsigned* scan) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    FlushConsoleInputBuffer(in);
    *none = *silent = false;
    for (;;) {
        INPUT_RECORD rec;
        DWORD n = 0;
        if (!ReadConsoleInputW(in, &rec, 1, &n) || !n) continue;
        if (rec.EventType == MOUSE_EVENT && rec.Event.MouseEvent.dwEventFlags == 0) {
            if (rec.Event.MouseEvent.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
                *none = true;
                return "";
            }
            if (rec.Event.MouseEvent.dwButtonState & RIGHTMOST_BUTTON_PRESSED) {
                *silent = true;
                return "";
            }
        }
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            const auto& k = rec.Event.KeyEvent;
            *vk = k.wVirtualKeyCode;
            *scan = k.wVirtualScanCode | (k.dwControlKeyState & ENHANCED_KEY ? 0x100 : 0);
            wchar_t name[64] = {};
            GetKeyNameTextW(static_cast<LONG>((k.wVirtualScanCode << 16) | (k.dwControlKeyState & ENHANCED_KEY ? 1 << 24 : 0)),
                            name, 64);
            char utf8[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, nullptr, nullptr);
            return utf8[0] ? utf8 : "?";
        }
    }
}

}  // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t appData[MAX_PATH];
    std::wstring dir = L".";
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH)) {
        dir = std::wstring(appData) + L"\\LumaBridge";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    const std::wstring outPath = dir + L"\\azoth-probe.txt";
    g_out = _wfopen(outPath.c_str(), L"w");

    Say("");
    Say("  ROG Azoth: per-key lighting test");
    Say("  --------------------------------");
    Say("  Close LumaBridge and Armoury Crate first. Nothing is saved to the keyboard.");
    g_dev = OpenAzoth(azoth::Link::Wired);
    if (g_dev == INVALID_HANDLE_VALUE) {
        g_link = azoth::Link::Wireless;
        g_dev = OpenAzoth(g_link);
    }
    if (g_dev == INVALID_HANDLE_VALUE) {
        Say("  The Azoth wasn't found (by cable or its Omni receiver). Is it on?");
        Ask("  Press Enter to close.");
        return 1;
    }
    Say("  Found it (%s).", g_link == azoth::Link::Wired ? "by cable" : "through the Omni receiver");
    Say("");

    // 1: every key green with the per-key command.
    Say("  Step 1: every key should turn GREEN.");
    SetKeys(kLeds, [](int, void*) { return Rgb3{0, 255, 0}; }, nullptr);
    std::string a1 = Ask("  Did the keys turn green? (y = all, s = some, n = none):");
    if (a1 == "n" || a1 == "N") {
        // Maybe the keyboard only shows per-key colors in static mode: switch to static black first.
        Say("  Trying again after switching the keyboard to a plain color...");
        Write({0x51, 0x2C, 0x00, 0x00, 0xFF, 0x64, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00});
        Sleep(100);
        SetKeys(kLeds, [](int, void*) { return Rgb3{0, 255, 0}; }, nullptr);
        a1 = Ask("  Green now? (y / s / n):");
        if (a1 == "n" || a1 == "N") {
            Say("");
            Say("  The keyboard didn't take the per-key command. Please send me the file below; the next");
            Say("  step is a USB capture of Armoury Crate's per-key lighting.");
            Say("  %ls", outPath.c_str());
            Ask("  Press Enter to close.");
            return 0;
        }
    }

    // 2: a rainbow by LED number, to see how the numbers run.
    Say("");
    Say("  Step 2: a rainbow by LED number (red, yellow, green, cyan, blue, purple).");
    SetKeys(kLeds, [](int i, void*) { return Hue(static_cast<float>(i) / 96.f); }, nullptr);
    Ask("  How does it run? (e.g. \"left to right\", \"top to bottom, then next column\"):");

    // 3: the map.
    Say("");
    Say("  Step 3: the map. One key at a time turns RED, the others dim blue.");
    Say("    Press the red key.");
    Say("    Left click in this window if no key is red.");
    Say("    Right click if the red key types nothing (like Fn).");
    Say("    The Windows key opens Start: press it, press Esc, then click this window's title bar.");
    Say("  Takes a few minutes; keep this window in front.");
    Ask("  Press Enter to start.");
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(in, &mode);
    SetConsoleMode(in, ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT);  // clicks, no QuickEdit

    struct Entry {
        int led;
        std::string key;
        unsigned vk, scan;
        bool silent;
    };
    std::vector<Entry> map;
    int empty = 0;
    for (int led = 0; led < kLeds; ++led) {
        SetKeys(kLeds, [](int i, void* ctx) { return i == *static_cast<int*>(ctx) ? Rgb3{255, 0, 0} : Rgb3{0, 0, 40}; },
                &led);
        bool none = false, silent = false;
        unsigned vk = 0, scan = 0;
        printf("  LED %3d: ", led);
        fflush(stdout);
        const std::string key = WaitKey(&none, &silent, &vk, &scan);
        if (none) {
            printf("nothing lit\n");
            if (g_out) fprintf(g_out, "  LED %3d: nothing lit\n", led);
            // Past the last key: stop after a long run of unlit numbers.
            if (++empty >= 24 && !map.empty()) break;
            continue;
        }
        empty = 0;
        const std::string name = silent ? "(types nothing)" : key;
        printf("%s\n", name.c_str());
        if (g_out) fprintf(g_out, "  LED %3d: %s (vk %02X, scan %03X)\n", led, name.c_str(), vk, scan);
        map.push_back({led, name, vk, scan, silent});
    }
    SetConsoleMode(in, mode);

    const std::wstring mapPath = dir + L"\\azoth-map.txt";
    if (FILE* f = _wfopen(mapPath.c_str(), L"w")) {
        fprintf(f, "# ROG Azoth: LED number -> key (vk, scan code)\n");
        for (const Entry& e : map) fprintf(f, "%d\t%s\t%02X\t%03X\n", e.led, e.key.c_str(), e.vk, e.scan);
        fclose(f);
    }
    // Leave it one color.
    SetKeys(kLeds, [](int, void*) { return Rgb3{255, 255, 255}; }, nullptr);
    Say("");
    Say("  Done: %zu keys mapped%s. Send me this file:", map.size(), g_writeErrors ? " (some writes failed)" : "");
    Say("    %ls", outPath.c_str());
    Say("  Restart LumaBridge or Armoury Crate to get your lighting back.");
    Ask("  Press Enter to close.");
    CloseHandle(g_dev);
    if (g_out) fclose(g_out);
    return 0;
}
