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
constexpr uint64_t kBluetoothStartupMs = 5000;  // let Sony's connection animation finish before the LED reset

// Opens the Sony HID collection with the output-report size for USB or Bluetooth. Other
// collections on the same controller (such as its audio controls) are skipped.
HANDLE OpenDualSense(DualSenseOutput::Link* link, USHORT* featureLength, USHORT* outputLength) {
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
        if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
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
        if (!match || !gotCaps || caps.UsagePage != 0x01 || caps.Usage != 0x05) continue;
        if (dualsense::UsbCollection(caps.InputReportByteLength, caps.OutputReportByteLength))
            *link = DualSenseOutput::Link::Usb;
        else if (dualsense::BluetoothCollection(caps.InputReportByteLength, caps.OutputReportByteLength))
            *link = DualSenseOutput::Link::Bluetooth;
        else
            continue;
        *outputLength = caps.OutputReportByteLength;
        *featureLength = caps.FeatureReportByteLength;
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
    uint64_t nextFind = 0, lastSent = 0, bluetoothResetAt = 0;
    USHORT featureLength = 0, outputLength = 0;
    uint8_t sequence = 0;
    bool bluetoothReady = false;
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
            bluetoothReady = false;
            if (dev != INVALID_HANDLE_VALUE) {
                CloseHandle(dev);
                dev = INVALID_HANDLE_VALUE;
                nextFind = 0;
            }
            continue;
        }
        if (dev == INVALID_HANDLE_VALUE) {
            if (now < nextFind) continue;
            dev = OpenDualSense(&link, &featureLength, &outputLength);
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
            sequence = 0;
            bluetoothReady = false;
            bluetoothResetAt = now + kBluetoothStartupMs;
            if (link == Link::Bluetooth) state_ = State::Connecting;
            if (link == Link::Bluetooth && featureLength > 0 && featureLength <= 4096) {
                // Reading the pairing-info feature enables full Bluetooth reports on Sony's
                // controller. The HID collection supplies the buffer length Windows expects.
                std::vector<BYTE> feature(featureLength);
                feature[0] = 0x09;
                if (!HidD_GetFeature(dev, feature.data(), static_cast<ULONG>(feature.size())))
                    LUMA_INFO("DualSense: Bluetooth feature request failed (error %lu); trying the output handshake", GetLastError());
            }
            lastSent = 0;
            lastWriteError_ = 0;
        }
        if (link == Link::Bluetooth && !bluetoothReady && now < bluetoothResetAt) continue;
        const Rgb color = Scale(fx::Render(effect, fx::Seconds(effect, now, since), 0, 1), brightness);
        if (lastSent && color == lastColor && now - lastSent < 3000) continue;  // resend now and then anyway
        DWORD written = 0;
        std::vector<uint8_t> buffer;
        const bool initializing = link == Link::Bluetooth && !bluetoothReady;
        if (link == Link::Usb) {
            const auto r = dualsense::UsbReport(color);
            buffer = dualsense::HidWriteBuffer(r, outputLength);
        } else {
            const auto r = initializing ? dualsense::BtResetReport(sequence++) : dualsense::BtReport(color, sequence++);
            buffer = dualsense::HidWriteBuffer(r, outputLength);
        }
        // Windows WriteFile expects the collection's maximum output length. The CRC stays
        // at bytes 74–77 of the actual Bluetooth packet, ahead of any zero padding.
        const bool ok = !buffer.empty() && WriteFile(dev, buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr) != 0;
        if (!ok || written != buffer.size()) {
            const DWORD err = ok ? ERROR_WRITE_FAULT : GetLastError();
            LUMA_WARN("DualSense: write failed (error %lu) - unplugged or out of range?", err);
            lastWriteError_ = err;
            CloseHandle(dev);
            dev = INVALID_HANDLE_VALUE;
            nextFind = now + 3000;
            state_ = State::NotFound;
            continue;
        }
        if (initializing) {
            bluetoothReady = true;
            LUMA_INFO("DualSense: Bluetooth lighting handshake sent");
            continue;  // give the reset a frame before sending the first color
        }
        lastColor = color;
        lastSent = now;
        state_ = State::Active;
    }
    if (dev != INVALID_HANDLE_VALUE) CloseHandle(dev);
}

}  // namespace luma::app
