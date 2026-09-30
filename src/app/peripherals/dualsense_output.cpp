#include "dualsense_output.h"

#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <vector>

#include "dualsense_protocol.h"
#include "log.h"

namespace luma::app {
namespace {

constexpr DWORD kFrameMs = 50;  // ~20 updates a second: plenty for a single-color lightbar

// Opens the DualSense's HID interface for writing, by USB or Bluetooth. Matches on vendor and
// product ID only (not a specific usage page/report length): DualSense and DualSense Edge
// expose several HID collections, and the output report itself carries its own report ID, so
// a mismatched interface just fails to open or write rather than light the wrong thing.
HANDLE OpenDualSense(DualSenseOutput::Link* link) {
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
        const bool match = HidD_GetAttributes(q, &a) && a.VendorID == dualsense::kVendor &&
                           (a.ProductID == dualsense::kProductStandard || a.ProductID == dualsense::kProductEdge);
        PHIDP_PREPARSED_DATA pre = nullptr;
        HIDP_CAPS caps{};
        const bool gotCaps = match && HidD_GetPreparsedData(q, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS;
        if (pre) HidD_FreePreparsedData(pre);
        CloseHandle(q);
        // The USB report is 48 bytes (including the report ID); over Bluetooth, Windows'
        // vendor collection for it reports 78. Anything else is a different collection on the
        // same device (the gamepad's own HID interface, audio control, ...) - skip it.
        if (match && gotCaps && caps.OutputReportByteLength == dualsense::kUsbReportSize) {
            *link = DualSenseOutput::Link::Usb;
        } else if (match && gotCaps && caps.OutputReportByteLength == dualsense::kBtReportSize) {
            *link = DualSenseOutput::Link::Bluetooth;
        } else {
            continue;
        }
        found = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

}  // namespace

void DualSenseOutput::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread(&DualSenseOutput::Run, this);
}

void DualSenseOutput::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    state_ = State::Off;
}

void DualSenseOutput::Set(const fx::Params& effect, double brightness, bool own) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (effect_.kind != effect.kind || effect_.color1 != effect.color1 || effect_.color2 != effect.color2 ||
        effect_.speed != effect.speed)
        effectSince_ = GetTickCount64();
    effect_ = effect;
    brightness_ = brightness;
    own_ = own;
}

void DualSenseOutput::Run() {
    HANDLE dev = INVALID_HANDLE_VALUE;
    Link link = Link::Usb;
    uint64_t nextFind = 0, lastSent = 0;
    bool loggedMissing = false;
    Rgb lastColor{};
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
            dev = OpenDualSense(&link);
            if (dev == INVALID_HANDLE_VALUE) {
                if (!loggedMissing) LUMA_INFO("DualSense: not found (neither by USB nor Bluetooth)");
                loggedMissing = true;
                lastWriteError_ = 0;
                state_ = State::NotFound;
                nextFind = now + 3000;
                continue;
            }
            LUMA_INFO("DualSense: connected (%s)", link == Link::Usb ? "USB" : "Bluetooth");
            link_ = link;
            loggedMissing = false;
            lastSent = 0;
            lastWriteError_ = 0;
        }
        const Rgb color = Scale(fx::Render(effect, fx::Seconds(effect, now, since), 0, 1), brightness);
        if (color == lastColor && now - lastSent < 3000) continue;  // resend now and then anyway
        bool ok;
        DWORD written = 0;
        if (link == Link::Usb) {
            const auto r = dualsense::UsbReport(color);
            ok = WriteFile(dev, r.data(), static_cast<DWORD>(r.size()), &written, nullptr) != 0;
        } else {
            const auto r = dualsense::BtReport(color);
            ok = WriteFile(dev, r.data(), static_cast<DWORD>(r.size()), &written, nullptr) != 0;
        }
        if (!ok) {
            const DWORD err = GetLastError();
            LUMA_WARN("DualSense: write failed (error %lu) - unplugged or out of range?", err);
            lastWriteError_ = err;
            CloseHandle(dev);
            dev = INVALID_HANDLE_VALUE;
            nextFind = now + 3000;
            state_ = State::NotFound;
            continue;
        }
        lastColor = color;
        lastSent = now;
        state_ = State::Active;
    }
    if (dev != INVALID_HANDLE_VALUE) CloseHandle(dev);
}

}  // namespace luma::app
