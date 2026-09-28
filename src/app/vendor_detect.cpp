#include "vendor_detect.h"

#include <windows.h>
#include <setupapi.h>

#include <string>
#include <utility>
#include <vector>

#include "log.h"
#include "setup_hardware.h"

namespace luma::app {
namespace {

using setup::App;
using setup::Brand;

void AddUsbDevices(setup::Answers* a) {
    for (const auto& [vid, pid] : UsbDevices()) setup::AddUsbDevice(a, vid, pid);
}

bool AnyFolder(std::initializer_list<const wchar_t*> paths) {
    for (const wchar_t* p : paths) {
        wchar_t full[MAX_PATH * 2];
        if (!ExpandEnvironmentStringsW(p, full, MAX_PATH * 2)) continue;
        const DWORD attr = GetFileAttributesW(full);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    }
    return false;
}

bool AnyService(std::initializer_list<const wchar_t*> names) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    bool found = false;
    for (const wchar_t* n : names) {
        if (SC_HANDLE s = OpenServiceW(scm, n, SERVICE_QUERY_STATUS)) {
            CloseServiceHandle(s);
            found = true;
            break;
        }
    }
    CloseServiceHandle(scm);
    return found;
}

}  // namespace

// Every USB device present, as (vendor, product) (its instance ID holds both).
std::vector<std::pair<uint16_t, uint16_t>> UsbDevices() {
    std::vector<std::pair<uint16_t, uint16_t>> out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof info;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        wchar_t id[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &info, id, 512, nullptr)) continue;
        uint16_t vid = 0, pid = 0;
        if (setup::ParseVidPid(id, &vid, &pid)) out.emplace_back(vid, pid);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

bool UsbDevicePresent(uint16_t vid, std::initializer_list<uint16_t> pids) {
    for (const auto& [v, p] : UsbDevices())
        if (v == vid)
            for (uint16_t want : pids)
                if (p == want) return true;
    return false;
}

setup::Answers DetectVendors(const SetupHardware& hw, bool auraFound) {
    setup::Answers a;
    AddUsbDevices(&a);
    if (auraFound || hw.board != BoardStyle::Generic || hw.boardName.find("ASUS") != std::string::npos)
        a.set(Brand::Asus);
    if (hw.ram != RamStyle::Generic) a.set(Brand::FuryRam);

    if (AnyService({L"ArmouryCrateService", L"LightingService"}) ||
        AnyFolder({L"%ProgramFiles%\\ASUS\\ARMOURY CRATE Service", L"%ProgramFiles(x86)%\\ASUS\\ArmouryDevice"}))
        a.set(App::ArmouryCrate);
    if (AnyFolder({L"%ProgramFiles%\\LGHUB"})) a.set(App::GHub);
    if (AnyFolder({L"%ProgramFiles(x86)%\\Razer\\Synapse3", L"%ProgramFiles%\\Razer\\RazerAppEngine",
                   L"%ProgramFiles(x86)%\\Razer\\RazerAppEngine"}))
        a.set(App::Synapse);
    if (AnyFolder({L"%ProgramFiles%\\SteelSeries\\GG", L"%ProgramFiles%\\SteelSeries\\SteelSeries Engine 3"}))
        a.set(App::SteelSeriesGG);
    if (AnyFolder({L"%ProgramFiles%\\Corsair\\CORSAIR iCUE 5 Software", L"%ProgramFiles%\\Corsair\\CORSAIR iCUE 4 Software",
                   L"%ProgramFiles(x86)%\\Corsair\\CORSAIR iCUE Software"}))
        a.set(App::Icue);
    if (AnyFolder({L"%ProgramFiles%\\Alienware\\Alienware Command Center",
                   L"%ProgramFiles(x86)%\\Alienware\\Alienware Command Center"}))
        a.set(App::AlienwareCC);

    std::string found;
    for (int i = 0; i < setup::kBrands; ++i)
        if (a.brands[i]) found += std::string(found.empty() ? "" : ", ") + setup::Info(static_cast<Brand>(i)).name;
    for (int i = 0; i < setup::kApps; ++i)
        if (a.apps[i]) found += std::string(found.empty() ? "" : ", ") + setup::Info(static_cast<App>(i)).name;
    LUMA_INFO("setup guide: found %s", found.empty() ? "nothing" : found.c_str());
    return a;
}

}  // namespace luma::app
