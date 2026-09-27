// READ-ONLY survey of HID devices, for adding keyboard / mouse lighting (ASUS ROG Azoth,
// Logitech G502 X Plus, ...). Lists every HID interface of ASUS (0B05) and Logitech (046D)
// devices - or of all devices with --all - with its usage page, report sizes and report IDs.
//
//   device-probe [--all]
//
// Nothing is sent to any device: interfaces are opened with no read/write access (enough
// for their descriptors), and only descriptor information is queried.
// Of special interest: usage page 0x59 "Lighting and Illumination" = HID LampArray, the
// standard interface Windows Dynamic Lighting uses. A device that has it can be lit through
// that documented protocol.
// Output: the console and %LOCALAPPDATA%\LumaBridge\device-probe.txt.
#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <cstdarg>
#include <cstdio>
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
    if (g_out) fprintf(g_out, "%s\n", buf);
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

const char* PageName(USHORT page) {
    switch (page) {
    case 0x01: return "Generic Desktop";
    case 0x07: return "Keyboard";
    case 0x08: return "LED";
    case 0x0C: return "Consumer";
    case 0x59: return "Lighting and Illumination (LampArray / Windows Dynamic Lighting)";
    default: return page >= 0xFF00 ? "Vendor-defined" : "";
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool all = argc > 1 && _wcsicmp(argv[1], L"--all") == 0;
    wchar_t appData[MAX_PATH];
    std::wstring outPath = L"device-probe.txt";
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH)) {
        std::wstring dir = std::wstring(appData) + L"\\LumaBridge";
        CreateDirectoryW(dir.c_str(), nullptr);
        outPath = dir + L"\\device-probe.txt";
    }
    g_out = _wfopen(outPath.c_str(), L"w");
    Log("LumaBridge device probe (read-only: descriptors only, nothing is sent to devices)");

    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        Log("SetupDiGetClassDevs failed (error %lu)", GetLastError());
        return 1;
    }
    int shown = 0, lampArrays = 0;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr)) continue;
        const std::wstring path = detail->DevicePath;

        // No access rights: descriptors only, and works even for keyboards the system holds.
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof attr;
        if (!HidD_GetAttributes(h, &attr) || (!all && attr.VendorID != 0x0B05 && attr.VendorID != 0x046D)) {
            CloseHandle(h);
            continue;
        }
        wchar_t product[128] = {}, maker[128] = {};
        HidD_GetProductString(h, product, sizeof product);
        HidD_GetManufacturerString(h, maker, sizeof maker);
        PHIDP_PREPARSED_DATA pre = nullptr;
        HIDP_CAPS caps{};
        const bool haveCaps = HidD_GetPreparsedData(h, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS;

        Log("");
        Log("%04X:%04X rev %04X  %s %s", attr.VendorID, attr.ProductID, attr.VersionNumber, Narrow(maker).c_str(),
            Narrow(product).c_str());
        Log("  %s", Narrow(path).c_str());
        if (haveCaps) {
            Log("  usage page 0x%04X usage 0x%04X  %s", caps.UsagePage, caps.Usage, PageName(caps.UsagePage));
            Log("  report bytes: input %u, output %u, feature %u", caps.InputReportByteLength,
                caps.OutputReportByteLength, caps.FeatureReportByteLength);
            // Report IDs and usages of the feature / output values (for LampArray these name
            // the attribute, lamp and update reports).
            for (HIDP_REPORT_TYPE type : {HidP_Feature, HidP_Output}) {
                USHORT n = type == HidP_Feature ? caps.NumberFeatureValueCaps : caps.NumberOutputValueCaps;
                if (!n) continue;
                std::vector<HIDP_VALUE_CAPS> vc(n);
                if (HidP_GetValueCaps(type, vc.data(), &n, pre) != HIDP_STATUS_SUCCESS) continue;
                std::string line;
                for (USHORT k = 0; k < n && k < 24; ++k) {
                    char item[48];
                    snprintf(item, sizeof item, " id%u:%02X/%04X", vc[k].ReportID, vc[k].UsagePage,
                             vc[k].IsRange ? vc[k].Range.UsageMin : vc[k].NotRange.Usage);
                    line += item;
                }
                Log("  %s values:%s%s", type == HidP_Feature ? "feature" : "output", line.c_str(), n > 24 ? " ..." : "");
            }
            if (caps.UsagePage == 0x59) ++lampArrays;
        }
        if (pre) HidD_FreePreparsedData(pre);
        CloseHandle(h);
        ++shown;
    }
    SetupDiDestroyDeviceInfoList(set);
    Log("");
    Log("%d interface(s) listed, %d LampArray interface(s). Saved to %s", shown, lampArrays, Narrow(outPath).c_str());
    if (g_out) fclose(g_out);
    return 0;
}
