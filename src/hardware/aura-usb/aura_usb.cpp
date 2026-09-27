#include "aura_usb.h"

#include <setupapi.h>

#include <iterator>

extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

namespace luma::aurausb {

std::vector<DeviceInfo> FindControllers(uint16_t vid, uint16_t pid) {
    std::vector<DeviceInfo> out;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;

    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
        std::vector<BYTE> buf(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, nullptr)) continue;

        // Zero access is enough to query attributes, even for devices opened exclusively.
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof attr;
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == vid && attr.ProductID == pid) {
            PHIDP_PREPARSED_DATA pp = nullptr;
            if (HidD_GetPreparsedData(h, &pp)) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage >= 0xFF00 &&
                    caps.OutputReportByteLength == kReportSize) {
                    DeviceInfo d;
                    d.path = detail->DevicePath;
                    d.vid = attr.VendorID;
                    d.pid = attr.ProductID;
                    d.usagePage = caps.UsagePage;
                    wchar_t product[128] = {};
                    if (HidD_GetProductString(h, product, sizeof product)) d.product = product;
                    out.push_back(d);
                }
                HidD_FreePreparsedData(pp);
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

bool Device::Open(const std::wstring& path) {
    Close();
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) return false;
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return event_ != nullptr;
}

void Device::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    if (event_) CloseHandle(event_);
    handle_ = INVALID_HANDLE_VALUE;
    event_ = nullptr;
}

namespace {

// Runs one overlapped read or write with a timeout; cancels it on timeout.
bool Overlapped(HANDLE h, HANDLE ev, bool write, BYTE* data, DWORD len, DWORD timeoutMs) {
    OVERLAPPED ov{};
    ov.hEvent = ev;
    ResetEvent(ev);
    DWORD done = 0;
    BOOL ok = write ? WriteFile(h, data, len, nullptr, &ov) : ReadFile(h, data, len, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(ev, timeoutMs) != WAIT_OBJECT_0) {
        CancelIo(h);
        GetOverlappedResult(h, &ov, &done, TRUE);  // wait for the cancel to finish
        return false;
    }
    return GetOverlappedResult(h, &ov, &done, FALSE) && done == len;
}

}  // namespace

bool Device::Write(const Report& r, DWORD timeoutMs) {
    if (!IsOpen()) return false;
    Report copy = r;
    return Overlapped(handle_, event_, true, copy.data(), static_cast<DWORD>(copy.size()), timeoutMs);
}

bool Device::Read(Report* r, DWORD timeoutMs) {
    if (!IsOpen()) return false;
    return Overlapped(handle_, event_, false, r->data(), static_cast<DWORD>(r->size()), timeoutMs);
}

bool Device::Transact(const Report& req, uint8_t replyCmd, Report* reply, DWORD timeoutMs) {
    if (!Write(req, timeoutMs)) return false;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        const DWORD left = static_cast<DWORD>(deadline - GetTickCount64());
        if (!Read(reply, left)) return false;
        if ((*reply)[0] == kReportId && (*reply)[1] == replyCmd) return true;
    }
    return false;
}

}  // namespace luma::aurausb
