#include "nzxt_kraken.h"

#include <windows.h>
#include <setupapi.h>

extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <string>
#include <vector>

#include "log.h"

namespace luma::app::nzxt {
namespace {

// The Kraken's HID interface (its path), or "" if it isn't plugged in.
std::wstring FindPath(uint16_t pid) {
    std::wstring found;
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return found;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; found.empty() && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr)) continue;
        HANDLE q = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof attr;
        if (HidD_GetAttributes(q, &attr) && attr.VendorID == kVid && attr.ProductID == pid) found = detail->DevicePath;
        CloseHandle(q);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

}  // namespace

void Kraken::Start(uint16_t pid, bool screen) {
    if (thread_.joinable() || !pid) return;
    stop_ = false;
    thread_ = std::thread(&Kraken::Run, this, pid, screen);
}

void Kraken::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = {};
}

void Kraken::Run(uint16_t pid, bool screen) {
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    uint64_t nextLook = 0;
    bool logged = false;
    DWORD inLen = 64, outLen = 64;  // report sizes (with the report ID), from the device
    auto nap = [&](int ms) {
        for (int t = 0; t < ms && !stop_; t += 100) Sleep(100);
    };
    while (!stop_) {
        if (h == INVALID_HANDLE_VALUE) {
            const uint64_t now = GetTickCount64();
            if (now < nextLook) {
                nap(500);
                continue;
            }
            nextLook = now + 10000;
            const std::wstring path = FindPath(pid);
            if (path.empty()) continue;
            h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                if (!logged) LUMA_WARN("NZXT Kraken: couldn't open it (error %lu)", GetLastError());
                logged = true;
                continue;
            }
            PHIDP_PREPARSED_DATA pre = nullptr;
            HIDP_CAPS caps{};
            if (HidD_GetPreparsedData(h, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS) {
                if (caps.InputReportByteLength) inLen = std::min<DWORD>(caps.InputReportByteLength, 256);
                if (caps.OutputReportByteLength) outLen = std::min<DWORD>(caps.OutputReportByteLength, 256);
            }
            if (pre) HidD_FreePreparsedData(pre);
            LUMA_INFO("NZXT Kraken %04X: reading its status (reports %lu / %lu bytes)", pid, inLen, outLen);
        }
        // Screen models answer a status request; the X models report on their own.
        if (screen) {
            uint8_t req[256] = {0x74, 0x01};
            OVERLAPPED ow{};
            ow.hEvent = ev;
            ResetEvent(ev);
            DWORD done = 0;
            if (!WriteFile(h, req, outLen, nullptr, &ow) && GetLastError() == ERROR_IO_PENDING &&
                WaitForSingleObject(ev, 500) != WAIT_OBJECT_0)
                CancelIo(h);
            GetOverlappedResult(h, &ow, &done, FALSE);
        }
        Status got;
        const uint64_t until = GetTickCount64() + 1200;
        while (!got.valid && GetTickCount64() < until && !stop_) {
            uint8_t msg[256] = {};
            OVERLAPPED ov{};
            ov.hEvent = ev;
            ResetEvent(ev);
            DWORD n = 0;
            if (!ReadFile(h, msg, inLen, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
                LUMA_INFO("NZXT Kraken: unplugged");
                CloseHandle(h);
                h = INVALID_HANDLE_VALUE;
                break;
            }
            if (WaitForSingleObject(ev, 500) != WAIT_OBJECT_0) {
                CancelIo(h);
                GetOverlappedResult(h, &ov, &n, TRUE);
                continue;
            }
            if (!GetOverlappedResult(h, &ov, &n, FALSE)) continue;
            got = Parse(msg, n, screen);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (got.valid || h == INVALID_HANDLE_VALUE) status_ = got;
        }
        nap(1000);
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CloseHandle(ev);
}

}  // namespace luma::app::nzxt
