#include "azoth_output.h"

#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <string>
#include <vector>

#include "azoth_protocol.h"
#include "log.h"

namespace luma::app {
namespace {

constexpr DWORD kFrameMs = 100;          // ~10 color updates per second at most
constexpr uint64_t kRefreshMs = 5000;    // re-send the same color now and then

// Opens the Azoth's lighting interface (0B05:1A83, usage page 0xFF00) for writing.
HANDLE OpenAzoth() {
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
        bool match = HidD_GetAttributes(q, &a) && a.VendorID == azoth::kVendor && a.ProductID == azoth::kProductWired;
        if (match) {
            PHIDP_PREPARSED_DATA pre = nullptr;
            HIDP_CAPS caps{};
            match = HidD_GetPreparsedData(q, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == azoth::kUsagePage && caps.OutputReportByteLength == azoth::kReportSize;
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

}  // namespace

void AzothOutput::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread(&AzothOutput::Run, this);
}

void AzothOutput::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    state_ = State::Off;
}

void AzothOutput::Set(const fx::Params& effect, double brightness, bool own) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (effect.kind != effect_.kind || effect.speed != effect_.speed) effectSince_ = GetTickCount64();
    effect_ = effect;
    brightness_ = brightness;
    own_ = own;
}

void AzothOutput::Run() {
    HANDLE dev = INVALID_HANDLE_VALUE;
    uint64_t nextFind = 0, lastSent = 0;
    Rgb last{1, 2, 3};
    bool loggedMissing = false;
    while (!stop_) {
        Sleep(kFrameMs);
        fx::Params effect;
        double brightness;
        bool own;
        uint64_t since;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            effect = effect_;
            brightness = brightness_;
            own = own_;
            since = effectSince_;
        }
        const uint64_t now = GetTickCount64();
        if (!own) {
            state_ = State::Released;
            lastSent = 0;
            continue;
        }
        if (dev == INVALID_HANDLE_VALUE) {
            if (now < nextFind) continue;
            dev = OpenAzoth();
            if (dev == INVALID_HANDLE_VALUE) {
                if (!loggedMissing) LUMA_INFO("ROG Azoth: not found on USB (plug it in by cable)");
                loggedMissing = true;
                state_ = State::NotFound;
                nextFind = now + 3000;
                continue;
            }
            LUMA_INFO("ROG Azoth: connected (wired)");
            loggedMissing = false;
            lastSent = 0;
        }
        const Rgb c = Scale(fx::Render(effect, static_cast<double>(now - since) / 1000.0, 0, 1), brightness);
        if (c == last && lastSent && now - lastSent < kRefreshMs) {
            state_ = State::Active;
            continue;
        }
        const azoth::Report r = azoth::StaticColor(c);
        if (azoth::IsSave(r)) continue;  // never write the keyboard's flash
        DWORD written = 0;
        if (!WriteFile(dev, r.data(), static_cast<DWORD>(r.size()), &written, nullptr)) {
            LUMA_WARN("ROG Azoth: write failed (error %lu) - unplugged?", GetLastError());
            CloseHandle(dev);
            dev = INVALID_HANDLE_VALUE;
            nextFind = now + 3000;
            state_ = State::NotFound;
            continue;
        }
        last = c;
        lastSent = now;
        state_ = State::Active;
    }
    if (dev != INVALID_HANDLE_VALUE) CloseHandle(dev);
}

}  // namespace luma::app
