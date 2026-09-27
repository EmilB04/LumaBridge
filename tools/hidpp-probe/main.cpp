// Logitech HID++ 2.0 lighting probe, for lighting a G502 X Plus (and other LIGHTSPEED mice)
// directly, the way G HUB does: through the receiver's HID++ collection.
//
//   hidpp-probe            read-only: lists the mouse's HID++ features and, for the RGB
//                          effects feature (0x8071), its clusters and their effects
//   hidpp-probe --test     also shows a few effects on the mouse (color wave, breathing,
//                          cycle, fixed), never saved to the mouse
//
// What G HUB sent (USB capture through the receiver 046D:C547, device index 1): long HID++
// reports "11 01 <feature> <function|swid> ...". SetRgbClusterEffect (0x8071 function 1):
//   <cluster> <effect index> <10 effect parameters> <persist>
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

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    wchar_t appData[MAX_PATH];
    std::wstring outPath = L"hidpp-probe.txt";
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH)) {
        std::wstring dir = std::wstring(appData) + L"\\LumaBridge";
        CreateDirectoryW(dir.c_str(), nullptr);
        outPath = dir + L"\\hidpp-probe.txt";
    }
    g_out = _wfopen(outPath.c_str(), L"w");
    Log("LumaBridge HID++ probe%s", test ? " (--test: shows effects, never saved)" : " (read-only)");

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
            uint8_t rgb = 0;
            for (uint8_t i = 0; i <= count[0]; ++i) {
                auto f = Request(d.h, dev, fsIndex, 1, {i});
                if (f.empty()) continue;
                const uint16_t id = static_cast<uint16_t>(f[0] << 8 | f[1]);
                Log("    [%02X] %04X %s (type %02X, version %u)", i, id, FeatureName(id), f[2], f[3]);
                if (id == 0x8071) rgb = i;
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

            if (!test) continue;
            // The effects as G HUB sent them, with persist = 0 (not saved to the mouse).
            Log("  Test (watch the mouse):");
            auto set = [&](std::initializer_list<uint8_t> p) {
                auto r = Request(d.h, dev, rgb, 1, p);
                Log("    %s", r.empty() ? "refused" : "accepted");
            };
            set({0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0x88, 0x01, 0x64, 0x13, 0x00});
            Pause("color wave, 5 s period?", 6000);
            set({0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0xD0, 0x01, 0x64, 0x07, 0x00});
            Pause("color wave, faster (2 s period)?", 6000);
            set({0x00, 0x02, 0xFF, 0x00, 0x00, 0x07, 0xD0, 0x00, 0x64, 0, 0, 0, 0x00});
            Pause("breathing red, 2 s?", 6000);
            set({0x00, 0x03, 0, 0, 0, 0, 0, 0x13, 0x88, 0x64, 0, 0, 0x00});
            Pause("color cycle, 5 s?", 6000);
            set({0x00, 0x01, 0x00, 0xFF, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0x00});
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
