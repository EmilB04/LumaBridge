#include "azoth_output.h"

#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <string>
#include <vector>

#include "azoth_layout.h"
#include "azoth_protocol.h"
#include "log.h"

namespace luma::app {
namespace {

constexpr DWORD kFrameMs = 40;           // ~25 updates per second at most (only changed keys are sent)
constexpr uint64_t kWirelessFrameMs = 50;   // through the Omni receiver: ~20 updates per second
constexpr uint64_t kRefreshMs = 5000;    // re-send every key now and then (a keyboard waking up)

// Opens the Azoth's lighting interface for writing: the keyboard's own (0B05:1A83) or the
// Omni receiver's (0B05:1ACE), usage page 0xFF00 with the link's output report size.
HANDLE OpenAzoth(azoth::Link link) {
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
        bool match = HidD_GetAttributes(q, &a) && a.VendorID == azoth::kVendor && a.ProductID == azoth::Product(link);
        if (match) {
            PHIDP_PREPARSED_DATA pre = nullptr;
            HIDP_CAPS caps{};
            match = HidD_GetPreparsedData(q, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == azoth::kUsagePage && caps.OutputReportByteLength == azoth::ReportSize(link);
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
    azoth::Link link = azoth::Link::Wired;
    uint64_t nextFind = 0, lastSent = 0;
    std::vector<Rgb> lastKeys;  // per key, as last sent
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
        // Asleep: nothing goes out (the dark frame went out before), so the keyboard can sleep.
        // Used again: every key again (lastSent 0 forces a full refresh).
        if (asleep_) {
            lastSent = 0;
            continue;
        }
        if (dev == INVALID_HANDLE_VALUE) {
            if (now < nextFind) continue;
            // The cable first: with both, the keyboard is plugged in and charging.
            link = azoth::Link::Wired;
            dev = OpenAzoth(link);
            if (dev == INVALID_HANDLE_VALUE) {
                link = azoth::Link::Wireless;
                dev = OpenAzoth(link);
            }
            if (dev == INVALID_HANDLE_VALUE) {
                if (!loggedMissing) LUMA_INFO("ROG Azoth: not found (neither by cable nor its Omni receiver)");
                loggedMissing = true;
                state_ = State::NotFound;
                nextFind = now + 3000;
                continue;
            }
            LUMA_INFO("ROG Azoth: connected (%s)", link == azoth::Link::Wired ? "wired" : "wireless, Omni receiver");
            link_ = link;
            loggedMissing = false;
            lastSent = 0;
        }
        const double t = fx::Seconds(effect, now, since);
        auto lost = [&] {
            LUMA_WARN("ROG Azoth: write failed (error %lu) - unplugged?", GetLastError());
            CloseHandle(dev);
            dev = INVALID_HANDLE_VALUE;
            nextFind = now + 3000;
            state_ = State::NotFound;
            lastKeys.clear();
        };
        // Every key its own color, by cable or through the Omni receiver. Over the receiver at
        // most 10 updates a second, sparing the 2.4 GHz link and the keyboard's battery.
        if (link == azoth::Link::Wireless && lastSent && now - lastSent < kWirelessFrameMs) continue;
        const std::vector<Rgb> keys = azoth::RenderKeys(effect, t, brightness);
        const bool refresh = now - lastSent >= kRefreshMs;  // everything again now and then
        if (keys == lastKeys && !refresh) {
            state_ = State::Active;
            continue;
        }
        std::vector<azoth::KeyColor> kc;
        const auto& layout = azoth::IsoKeys();
        for (size_t i = 0; i < layout.size(); ++i)
            if (lastKeys.empty() || refresh || keys[i] != lastKeys[i])
                kc.push_back({static_cast<uint8_t>(layout[i].led), keys[i]});
        bool ok = true;
        for (const azoth::Report& r : azoth::KeyColors(kc, link)) {
            DWORD written = 0;
            if (azoth::IsSave(r) || !WriteFile(dev, r.data(), static_cast<DWORD>(azoth::ReportSize(link)), &written, nullptr)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            lost();
            continue;
        }
        lastKeys = keys;
        lastSent = now;
        state_ = State::Active;
    }
    if (dev != INVALID_HANDLE_VALUE) CloseHandle(dev);
}

}  // namespace luma::app
