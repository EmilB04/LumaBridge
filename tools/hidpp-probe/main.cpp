// Logitech HID++ 2.0 lighting probe, for lighting a G502 X Plus (and other LIGHTSPEED mice)
// directly, the way G HUB does: through the receiver's HID++ collection.
//
//   hidpp-probe            read-only: lists the mouse's HID++ features and, for the RGB
//                          effects feature (0x8071), its clusters and their effects
//   hidpp-probe --test     also shows a few effects on the mouse (color wave, breathing,
//                          cycle, fixed), byte for byte as G HUB sends them
//   hidpp-probe --perkey   also tries coloring the LEDs one by one (per-key lighting
//                          0x8081: set zones, then end the frame)
//   hidpp-probe --map      interactive: lights one zone at a time and asks which LED lit
//                          (numbered as in G HUB's picture: 1-6 along the bottom from the
//                          thumb side, 7-8 up the right side), then checks the order with a
//                          moving dot. Saved to %LOCALAPPDATA%\LumaBridge\mouse-map.txt
//
// What G HUB sent (USB capture through the receiver 046D:C547, device index 1): long HID++
// reports "11 01 <feature> <function|swid> ...". SetRgbClusterEffect (0x8071 function 1):
//   <cluster> <effect index> <10 effect parameters> <01>
// The last byte: G HUB always sends 01. With 00 the mouse answers but doesn't change (tested
// on a G502 X Plus). G HUB reports zone effects as not stored on this mouse.
//   cluster 00 effect 01  R G B 02 ...               fixed
//   cluster 00 effect 02  R G B <period ms BE> 00 <intensity>   breathing
//   cluster 00 effect 03  00 00 00 00 00 <period ms BE> <intensity>   cycle
//   cluster FF effect 00  00 x6 <period lo> 01 <intensity> <period hi>   color wave
// Output: the console and %LOCALAPPDATA%\LumaBridge\hidpp-probe.txt.
#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <string>
#include <vector>

namespace {

FILE* g_out = nullptr;

void Log(const char* fmt, ...) {
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

std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof b, "%02X ", p[i]);
        s += b;
    }
    if (!s.empty()) s.pop_back();
    return s;
}

constexpr uint16_t kLogitech = 0x046D;
constexpr uint8_t kLong = 0x11;
constexpr uint8_t kSwId = 0x0A;  // our software ID (G HUB uses 0x0B)

struct Device {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint16_t pid = 0;
};

// Every Logitech HID++ long-report collection (usage page 0xFF00, 20-byte reports).
std::vector<Device> OpenAll() {
    std::vector<Device> out;
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
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
        bool match = HidD_GetAttributes(q, &a) && a.VendorID == kLogitech;
        if (match) {
            PHIDP_PREPARSED_DATA pre = nullptr;
            HIDP_CAPS caps{};
            match = HidD_GetPreparsedData(q, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == 0xFF00 && caps.OutputReportByteLength == 20;
            if (pre) HidD_FreePreparsedData(pre);
        }
        CloseHandle(q);
        if (!match) continue;
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) out.push_back(Device{h, a.ProductID});
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

bool Write(HANDLE h, const uint8_t (&r)[20]) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n = 0;
    BOOL ok = WriteFile(h, r, 20, &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0 && GetOverlappedResult(h, &ov, &n, FALSE);
    if (!ok) CancelIo(h);
    CloseHandle(ov.hEvent);
    return ok && n == 20;
}

// Reads reports until one answers (dev, feature, function) or `ms` pass. 0: answered,
// 1: HID++ error (code in err), -1: nothing.
int Read(HANDLE h, uint8_t dev, uint8_t feature, uint8_t fnSw, uint8_t (&reply)[20], uint8_t& err, DWORD ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= until) return -1;
        uint8_t r[20] = {};
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        BOOL ok = ReadFile(h, r, 20, &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, static_cast<DWORD>(until - now)) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(h, &ov, &n, FALSE);
            else
                CancelIo(h);
        }
        CloseHandle(ov.hEvent);
        if (!ok || n < 7 || r[0] != kLong || r[1] != dev) continue;
        if (r[2] == feature && r[3] == fnSw) {
            memcpy(reply, r, 20);
            return 0;
        }
        if (r[2] == 0xFF && r[3] == feature && r[4] == fnSw) {  // HID++ 2.0 error
            err = r[5];
            return 1;
        }
    }
}

// One HID++ request; returns the 16 parameter bytes of the answer, or empty.
std::vector<uint8_t> Request(HANDLE h, uint8_t dev, uint8_t feature, uint8_t fn, std::initializer_list<uint8_t> params,
                             bool quiet = false) {
    uint8_t r[20] = {kLong, dev, feature, static_cast<uint8_t>((fn << 4) | kSwId)};
    size_t i = 4;
    for (uint8_t p : params)
        if (i < 20) r[i++] = p;
    if (!Write(h, r)) {
        if (!quiet) Log("    write failed (error %lu): %s", GetLastError(), Hex(r, 20).c_str());
        return {};
    }
    uint8_t reply[20];
    uint8_t err = 0;
    const int got = Read(h, dev, feature, r[3], reply, err, 1500);
    if (got == 0) return std::vector<uint8_t>(reply + 4, reply + 20);
    if (!quiet) {
        if (got == 1)
            Log("    %s -> HID++ error %u", Hex(r, 8).c_str(), err);
        else
            Log("    %s -> no answer", Hex(r, 8).c_str());
    }
    return {};
}

const char* FeatureName(uint16_t id) {
    switch (id) {
    case 0x0000: return "Root";
    case 0x0001: return "Feature set";
    case 0x0003: return "Firmware info";
    case 0x0005: return "Device name";
    case 0x1000: return "Battery";
    case 0x1004: return "Unified battery";
    case 0x1D4B: return "Wireless status";
    case 0x1300: return "LED control";
    case 0x8070: return "Color LED effects";
    case 0x8071: return "RGB effects";
    case 0x8080: return "Per-key lighting";
    case 0x8081: return "Per-key lighting v2";
    case 0x8100: return "Onboard profiles";
    case 0x2201: return "Adjustable DPI";
    case 0x8060: return "Report rate";
    case 0x8061: return "Extended report rate";
    default: return "";
    }
}

void Pause(const char* what, DWORD ms) {
    Log("  %s", what);
    Sleep(ms);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool test = argc > 1 && _wcsicmp(argv[1], L"--test") == 0;
    const bool perkey = argc > 1 && _wcsicmp(argv[1], L"--perkey") == 0;
    const bool map = argc > 1 && _wcsicmp(argv[1], L"--map") == 0;
    wchar_t appData[MAX_PATH];
    std::wstring outPath = L"hidpp-probe.txt";
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH)) {
        std::wstring dir = std::wstring(appData) + L"\\LumaBridge";
        CreateDirectoryW(dir.c_str(), nullptr);
        outPath = dir + L"\\hidpp-probe.txt";
    }
    g_out = _wfopen(outPath.c_str(), L"w");
    Log("LumaBridge HID++ probe%s", test ? " (--test: shows effects)" : perkey ? " (--perkey: colors LEDs)" : map ? " (--map: maps the LEDs)" : " (read-only)");

    const auto devices = OpenAll();
    if (devices.empty()) {
        Log("No Logitech HID++ interface found.");
        return 1;
    }
    int found = 0;
    for (const Device& d : devices) {
        Log("");
        Log("Logitech 046D:%04X", d.pid);
        // Index 0xFF is a device on its own cable; 1-6 are devices paired to a receiver.
        for (uint8_t dev : {uint8_t{0xFF}, uint8_t{1}, uint8_t{2}, uint8_t{3}, uint8_t{4}, uint8_t{5}, uint8_t{6}}) {
            // Root.GetFeature(0x0001): answers only if a HID++ 2.0 device is there.
            auto fs = Request(d.h, dev, 0x00, 0, {0x00, 0x01}, true);
            if (fs.empty() || fs[0] == 0) continue;
            ++found;
            Log("  device index %u", dev);
            const uint8_t fsIndex = fs[0];
            auto name = Request(d.h, dev, 0x00, 0, {0x00, 0x05}, true);
            if (!name.empty() && name[0]) {
                auto len = Request(d.h, dev, name[0], 0, {});
                std::string n;
                for (uint8_t at = 0; !len.empty() && at < len[0];) {
                    auto part = Request(d.h, dev, name[0], 1, {at});
                    if (part.empty()) break;
                    for (uint8_t c : part)
                        if (at < len[0]) n += static_cast<char>(c), ++at;
                }
                Log("  name: %s", n.c_str());
            }
            auto count = Request(d.h, dev, fsIndex, 0, {});
            if (count.empty()) continue;
            Log("  %u features:", count[0]);
            uint8_t rgb = 0, perKey = 0;
            for (uint8_t i = 0; i <= count[0]; ++i) {
                auto f = Request(d.h, dev, fsIndex, 1, {i});
                if (f.empty()) continue;
                const uint16_t id = static_cast<uint16_t>(f[0] << 8 | f[1]);
                Log("    [%02X] %04X %s (type %02X, version %u)", i, id, FeatureName(id), f[2], f[3]);
                if (id == 0x8071) rgb = i;
                if (id == 0x8081) perKey = i;
            }
            if (!rgb) continue;

            // RGB effects: GetInfo(FF, FF) = the clusters; (c, FF) = a cluster; (c, e) = an effect.
            Log("  RGB effects (0x8071, index %02X):", rgb);
            auto info = Request(d.h, dev, rgb, 0, {0xFF, 0xFF});
            if (info.empty()) continue;
            Log("    info: %s", Hex(info.data(), info.size()).c_str());
            const int clusters = info[2] ? info[2] : 4;
            for (int c = 0; c < clusters; ++c) {
                auto ci = Request(d.h, dev, rgb, 0, {static_cast<uint8_t>(c), 0xFF}, true);
                if (ci.empty()) break;
                Log("    cluster %d: %s", c, Hex(ci.data(), ci.size()).c_str());
                for (int e = 0; e < 16; ++e) {
                    auto ei = Request(d.h, dev, rgb, 0, {static_cast<uint8_t>(c), static_cast<uint8_t>(e)}, true);
                    if (ei.empty()) break;
                    Log("      effect %d: %s", e, Hex(ei.data(), ei.size()).c_str());
                }
            }

            // The effects spanning the whole mouse (cluster FF).
            for (int e = 0; e < 8; ++e) {
                auto ei = Request(d.h, dev, rgb, 0, {0xFF, static_cast<uint8_t>(e)}, true);
                if (ei.empty()) break;
                Log("    cluster FF effect %d: %s", e, Hex(ei.data(), ei.size()).c_str());
            }

            // Per-key lighting: info (0, 0) answers "00 00 <bitmap of zone IDs>" (the G502 X Plus:
            // FF 01 = zones 0-8). A report with any zone the device lacks is refused whole.
            std::vector<int> zones;
            if (perKey) {
                Log("  Per-key lighting (0x8081, index %02X):", perKey);
                for (uint8_t a : {uint8_t{0}, uint8_t{1}, uint8_t{2}})
                    for (uint8_t b : {uint8_t{0}, uint8_t{1}}) {
                        auto pi = Request(d.h, dev, perKey, 0, {a, b}, true);
                        Log("    info %u %u: %s", a, b, pi.empty() ? "refused" : Hex(pi.data(), pi.size()).c_str());
                        if (a == 0 && b == 0 && !pi.empty())
                            for (int k = 2; k < 16; ++k)
                                for (int bit = 0; bit < 8; ++bit)
                                    if (pi[k] >> bit & 1) zones.push_back((k - 2) * 8 + bit);
                    }
                std::string list;
                for (int z : zones) list += std::to_string(z) + " ";
                Log("    zones: %s", list.c_str());
            }
            if ((perkey || map) && perKey && !zones.empty()) {
                using C = std::array<uint8_t, 3>;
                auto frame = [&](const char* what, auto zoneColor, DWORD ms = 5000) {
                    // The device's zones, four per report (zone R G B), then end the frame.
                    int accepted = 0, refused = 0;
                    for (size_t z = 0; z < zones.size(); z += 4) {
                        uint8_t r[16] = {};
                        size_t n = 0;
                        for (; n < 4 && z + n < zones.size(); ++n) {
                            const int id = zones[z + n];
                            const C c = zoneColor(id);
                            r[n * 4] = static_cast<uint8_t>(id);
                            r[n * 4 + 1] = c[0];
                            r[n * 4 + 2] = c[1];
                            r[n * 4 + 3] = c[2];
                        }
                        for (; n < 4; ++n) {  // pad with the first zone again, same color
                            memcpy(r + n * 4, r, 4);
                        }
                        auto a = Request(d.h, dev, perKey, 1,
                                         {r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]},
                                         true);
                        a.empty() ? ++refused : ++accepted;
                    }
                    auto end = Request(d.h, dev, perKey, 7, {0, 0, 0, 0}, true);
                    Log("    %d report(s) accepted, %d refused; frame end %s", accepted, refused, end.empty() ? "refused" : "accepted");
                    Pause(what, ms);
                };
                static const C rainbow[9] = {{255, 0, 0},   {255, 110, 0}, {255, 230, 0}, {0, 255, 0},  {0, 230, 255},
                                             {0, 40, 255},  {150, 0, 255}, {255, 0, 150}, {255, 255, 255}};
                auto white = [](int) { return C{0xFF, 0xFF, 0xFF}; };
                auto colors = [](int z) { return rainbow[z % 9]; };
                if (map) {
                    auto ask = [](const char* q) {
                        printf("%s ", q);
                        fflush(stdout);
                        char line[128] = {};
                        if (!fgets(line, sizeof line, stdin)) return std::string();
                        std::string s(line);
                        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
                        return s;
                    };
                    Log("  Mapping the LEDs. Press Enter for the default in [brackets].");
                    std::string mode = ask("  Which way worked in --perkey? 1 = A (nothing first), 2 = B, 3 = C [3]:");
                    Log("    mode: %s", mode.empty() ? "3" : mode.c_str());
                    if (mode.empty() || mode == "2" || mode == "3") {
                        const uint8_t e = mode == "2" ? 3 : 4;
                        auto r = Request(d.h, dev, rgb, 1, {0xFF, e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01});
                        Log("    whole-mouse effect %u %s", e, r.empty() ? "refused" : "accepted");
                    }
                    static const char* names[9] = {"red", "orange", "yellow", "green", "cyan", "blue", "purple", "pink", "white"};
                    std::string legend;
                    for (int z : zones) legend += "zone " + std::to_string(z) + " " + names[z % 9] + ", ";
                    frame("every zone its own color", colors, 0);
                    Log("    %s", legend.c_str());
                    Log("    you saw: %s", ask("  Which colors do you see, LED 1 to 8? (e.g. red orange yellow ...)").c_str());

                    Log("  One zone at a time. Number the LEDs like G HUB's picture: 1-6 along the bottom curve from the thumb side (left), 7-8 up the right side (7 lower, 8 upper).");
                    std::vector<std::pair<int, int>> area;  // (zone, area; 0 = nothing)
                    for (size_t i = 0; i < zones.size(); ++i) {
                        const int z = zones[i];
                        char what[48];
                        snprintf(what, sizeof what, "zone %d red, the rest off", z);
                        frame(what, [z](int k) { return k == z ? C{0xFF, 0, 0} : C{0, 0, 0}; }, 0);
                        std::string a = ask("    Which LED lit? (1-8; several: the first; 0 = nothing; r = show again):");
                        if (a == "r") {
                            --i;
                            continue;
                        }
                        const int n = atoi(a.c_str());
                        Log("    zone %d -> LED %d%s%s", z, n, a.empty() || isdigit(static_cast<unsigned char>(a[0])) ? "" : "  note: ",
                            a.empty() || isdigit(static_cast<unsigned char>(a[0])) ? "" : a.c_str());
                        area.push_back({z, n});
                    }
                    int areas = 0;
                    for (auto& [z, n] : area) areas = n > areas ? n : areas;
                    std::string summary;
                    for (int n = 1; n <= areas; ++n) {
                        summary += "LED " + std::to_string(n) + ": zone";
                        for (auto& [z, m] : area)
                            if (m == n) summary += " " + std::to_string(z);
                        summary += "; ";
                    }
                    Log("  Map: %s", summary.c_str());

                    if (areas > 0) {
                        Log("  Check: a red dot runs from LED 1 to LED %d, three times.", areas);
                        for (int round = 0; round < 3; ++round)
                            for (int n = 1; n <= areas; ++n) {
                                auto dot = [&](int k) {
                                    for (auto& [z, m] : area)
                                        if (z == k) return m == n ? C{0xFF, 0, 0} : C{0, 0, 0x30};
                                    return C{0, 0, 0};
                                };
                                char what[32];
                                snprintf(what, sizeof what, "LED %d", n);
                                frame(what, dot, 600);
                            }
                        Log("    %s", ask("  Did the dot run smoothly along the bottom and up the right side? (y / n + what you saw):").c_str());
                        Log("  Check: each LED its own color (1 red, 2 green, 3 blue, 4 yellow, 5 purple, 6 cyan, 7 red, 8 green).");
                        static const C areaColors[6] = {{255, 0, 0}, {0, 255, 0}, {0, 40, 255}, {255, 230, 0}, {150, 0, 255}, {0, 230, 255}};
                        frame("LEDs colored", [&](int k) {
                            for (auto& [z, m] : area)
                                if (z == k && m > 0) return areaColors[(m - 1) % 6];
                            return C{0, 0, 0};
                        }, 0);
                        Log("    %s", ask("  Does each LED show its own clean color? (y / n + what you saw):").c_str());
                    }
                    wchar_t appData2[MAX_PATH];
                    if (GetEnvironmentVariableW(L"LOCALAPPDATA", appData2, MAX_PATH)) {
                        std::wstring mp = std::wstring(appData2) + L"\\LumaBridge\\mouse-map.txt";
                        if (FILE* f = _wfopen(mp.c_str(), L"w")) {
                            fprintf(f, "G502 X PLUS zones -> LEDs (G HUB's numbering: 1-6 bottom from the thumb side, 7-8 right side)\n");
                            for (auto& [z, n] : area) fprintf(f, "zone %d LED %d\n", z, n);
                            fclose(f);
                            Log("  Saved the map to %ls", mp.c_str());
                        }
                    }
                    Log("  Mapping done. Pick your effect in G HUB again to get it back.");
                } else {
                Log("  A: straight away (watch the mouse):");
                frame("A1: all LEDs white?", white);
                frame("A2: every LED its own color?", colors);
                for (uint8_t e : {uint8_t{3}, uint8_t{4}}) {
                    Log("  %c: after switching the mouse to whole-mouse effect %u:", e == 3 ? 'B' : 'C', e);
                    auto r = Request(d.h, dev, rgb, 1, {0xFF, e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01});
                    Log("    effect %u %s", e, r.empty() ? "refused" : "accepted");
                    frame(e == 3 ? "B1: all LEDs white?" : "C1: all LEDs white?", white);
                    frame(e == 3 ? "B2: every LED its own color?" : "C2: every LED its own color?", colors);
                }
                Log("  One LED at a time (note which LED lights for each zone):");
                for (int z : zones) {
                    char what[48];
                    snprintf(what, sizeof what, "zone %d red, the rest off", z);
                    frame(what, [z](int k) { return k == z ? C{0xFF, 0, 0} : C{0, 0, 0}; }, 2500);
                }
                Log("  Per-key test done. Pick your effect in G HUB again to get it back.");
                }
            }

            if (!test) continue;
            // The effects byte for byte as G HUB sends them (last byte 01).
            Log("  Test (watch the mouse):");
            auto set = [&](std::initializer_list<uint8_t> p) {
                auto r = Request(d.h, dev, rgb, 1, p);
                Log("    %s", r.empty() ? "refused" : "accepted");
            };
            set({0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0x88, 0x01, 0x64, 0x13, 0x01});
            Pause("color wave, 5 s period?", 6000);
            set({0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0xD0, 0x01, 0x64, 0x07, 0x01});
            Pause("color wave, faster (2 s period)?", 6000);
            set({0x00, 0x02, 0xFF, 0x00, 0x00, 0x07, 0xD0, 0x00, 0x64, 0, 0, 0, 0x01});
            Pause("breathing red, 2 s?", 6000);
            set({0x00, 0x03, 0, 0, 0, 0, 0, 0x13, 0x88, 0x64, 0, 0, 0x01});
            Pause("color cycle, 5 s?", 6000);
            set({0x00, 0x01, 0x00, 0xFF, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0x01});
            Pause("fixed green?", 4000);
            Log("  Test done. Pick your effect in G HUB again to get it back.");
        }
    }
    for (const Device& d : devices) CloseHandle(d.h);
    if (!found) Log("No HID++ 2.0 device answered (is the mouse on and awake?).");
    Log("");
    Log("Saved to %ls", outPath.c_str());
    if (g_out) fclose(g_out);
    return 0;
}
