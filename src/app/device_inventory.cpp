#include "device_inventory.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
extern "C" {
#include <hidsdi.h>
}

#include "device_catalog.h"
#include "setup_plan.h"

namespace luma::app::inventory {
namespace {

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Property(HDEVINFO set, SP_DEVINFO_DATA& info, DWORD property) {
    DWORD bytes = 0, type = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &info, property, &type, nullptr, 0, &bytes);
    if (!bytes) return {};
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, &info, property, &type,
            reinterpret_cast<BYTE*>(value.data()), bytes, nullptr)) return {};
    if (type != REG_SZ && type != REG_MULTI_SZ) return {};
    return value.data();
}

}  // namespace

Scan ScanPresentDevices() {
    Scan out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        out.error = GetLastError();
        return out;
    }
    for (DWORD i = 0; ; ++i) {
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof info;
        if (!SetupDiEnumDeviceInfo(set, i, &info)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_ITEMS) out.error = error;
            break;
        }
        DWORD chars = 0;
        SetupDiGetDeviceInstanceIdW(set, &info, nullptr, 0, &chars);
        std::vector<wchar_t> id(chars + 1, L'\0');
        if (!SetupDiGetDeviceInstanceIdW(set, &info, id.data(), static_cast<DWORD>(id.size()), nullptr)) {
            if (!out.error) out.error = GetLastError();
            continue;
        }
        Device d;
        d.id = Utf8(id.data());
        d.name = Utf8(Property(set, info, SPDRP_FRIENDLYNAME));
        if (d.name.empty()) d.name = Utf8(Property(set, info, SPDRP_DEVICEDESC));
        if (d.name.empty()) d.name = d.id;
        d.setupClass = Utf8(Property(set, info, SPDRP_CLASS));
        d.className = KindName(d.setupClass);  // English, whatever language Windows runs in
        d.manufacturer = Utf8(Property(set, info, SPDRP_MFG));
        d.location = Utf8(Property(set, info, SPDRP_LOCATION_INFORMATION));
        d.category = Classify(d.setupClass);
        // Hardware IDs on Bluetooth HID nodes contain VID/PID too. Do not restrict this
        // inventory to USB or require opening a device for lighting to establish presence.
        if (!setup::ParseVidPid(id.data(), &d.vid, &d.pid)) {
            const auto hardwareId = Property(set, info, SPDRP_HARDWAREID);
            setup::ParseVidPid(hardwareId.c_str(), &d.vid, &d.pid);
        }
        for (const auto& aio : catalog::AioModels())
            if (d.vid == aio.vid && d.pid == aio.pid) { d.name = aio.name; d.category = Category::Cooling; break; }
        for (const auto& psu : catalog::PsuModels())
            if (d.vid == psu.vid && d.pid == psu.pid) { d.name = psu.name; d.category = Category::Cooling; break; }
        ULONG status = 0, problem = 0;
        d.statusKnown = CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) == CR_SUCCESS;
        if (d.statusKnown && (status & DN_HAS_PROBLEM)) d.problem = problem;
        out.devices.push_back(std::move(d));
    }
    SetupDiDestroyDeviceInfoList(set);
    // Bluetooth HID IDs need not encode VID/PID in USB notation. Ask HID for identity
    // using a metadata-only handle; this never sends lighting or feature reports.
    GUID hid{};
    HidD_GetHidGuid(&hid);
    set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set != INVALID_HANDLE_VALUE) {
        for (DWORD i = 0; ; ++i) {
            SP_DEVICE_INTERFACE_DATA iface{};
            iface.cbSize = sizeof iface;
            if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface)) break;
            DWORD bytes = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &bytes, nullptr);
            if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
            std::vector<BYTE> buffer(bytes);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof *detail;
            SP_DEVINFO_DATA info{};
            info.cbSize = sizeof info;
            if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, bytes, nullptr, &info)) continue;
            wchar_t id[MAX_DEVICE_ID_LEN]{};
            if (!SetupDiGetDeviceInstanceIdW(set, &info, id, MAX_DEVICE_ID_LEN, nullptr)) continue;
            const std::string key = Utf8(id);
            auto node = std::find_if(out.devices.begin(), out.devices.end(), [&](const Device& d) { return d.id == key; });
            if (node == out.devices.end() || (node->vid && node->pid)) continue;
            HANDLE handle = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE) continue;
            HIDD_ATTRIBUTES attributes{};
            attributes.Size = sizeof attributes;
            if (HidD_GetAttributes(handle, &attributes)) {
                node->vid = attributes.VendorID;
                node->pid = attributes.ProductID;
            }
            CloseHandle(handle);
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    Sort(out.devices);
    return out;
}

}  // namespace luma::app::inventory
