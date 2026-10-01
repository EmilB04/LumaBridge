#include "pad_input.h"

#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <vector>

#include "log.h"

namespace luma::app {
namespace {

struct Opened {
    HANDLE handle = INVALID_HANDLE_VALUE;
    pad::Model model = pad::Model::None;
    bool bluetooth = false;
    USHORT inputLength = 0, featureLength = 0;
};

// Opens the first Sony pad's gamepad collection for reading. Other collections of the same
// controller (audio controls, ...) are skipped. Bluetooth has the longer input report.
Opened OpenPad() {
    Opened out;
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; out.handle == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
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
        pad::Model model = HidD_GetAttributes(q, &a) ? pad::ModelOf(a.VendorID, a.ProductID) : pad::Model::None;
        PHIDP_PREPARSED_DATA pre = nullptr;
        HIDP_CAPS caps{};
        const bool gotCaps = model != pad::Model::None && HidD_GetPreparsedData(q, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS;
        if (pre) HidD_FreePreparsedData(pre);
        CloseHandle(q);
        if (!gotCaps || caps.UsagePage != 0x01 || caps.Usage != 0x05 || caps.InputReportByteLength < 64) continue;
        out.model = model;
        out.bluetooth = caps.InputReportByteLength > 64;
        out.inputLength = caps.InputReportByteLength;
        out.featureLength = caps.FeatureReportByteLength;
        out.handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (out.handle == INVALID_HANDLE_VALUE)  // read-only is enough
            out.handle = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

uint64_t NowUs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart) * 1000000ull / static_cast<uint64_t>(freq.QuadPart);
}

// Sends the mapped keys and mouse input. Keeps what it pressed so it can let go again.
class Sender {
public:
    ~Sender() { ReleaseAll(); }

    void Apply(const std::vector<pad::Event>& events) {
        for (const auto& e : events) Send(e.binding, e.down);
    }
    void Move(int dx, int dy) {
        if (!dx && !dy) return;
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = dx;
        in.mi.dy = dy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof in);
    }
    void ReleaseAll() {
        for (auto it = held_.begin(); it != held_.end();) {
            Send(*it, false, false);
            it = held_.erase(it);
        }
    }

private:
    void Send(const pad::Binding& b, bool down, bool track = true) {
        INPUT in{};
        if (b.action == pad::Action::Key) {
            in.type = INPUT_KEYBOARD;
            // Scan codes, so games that read the keyboard directly see the key too.
            const UINT scan = MapVirtualKeyW(b.code, MAPVK_VK_TO_VSC);
            in.ki.wScan = static_cast<WORD>(scan);
            in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
            if ((b.code >= 0x21 && b.code <= 0x28) || b.code == 0x2D || b.code == 0x2E) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        } else if (b.action == pad::Action::Mouse) {
            in.type = INPUT_MOUSE;
            switch (b.code) {
            case 1: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
            case 2: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
            case 3: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
            case 4: case 5:
                if (!down) return;  // one notch a press
                in.mi.dwFlags = MOUSEEVENTF_WHEEL;
                in.mi.mouseData = static_cast<DWORD>(b.code == 4 ? WHEEL_DELTA : -WHEEL_DELTA);
                break;
            default: return;
            }
        } else {
            return;
        }
        SendInput(1, &in, sizeof in);
        if (!track || (b.action == pad::Action::Mouse && b.code >= 4)) return;
        if (down) held_.push_back(b);
        else
            for (auto it = held_.begin(); it != held_.end(); ++it)
                if (*it == b) { held_.erase(it); break; }
    }

    std::vector<pad::Binding> held_;
};

}  // namespace

void PadInput::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread(&PadInput::Run, this);
}

void PadInput::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    snap_ = Snapshot();
}

PadInput::Snapshot PadInput::Get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

void PadInput::SetMapping(const pad::Mapping& mapping) {
    std::lock_guard<std::mutex> lock(mutex_);
    mapping_ = mapping;
}

void PadInput::Run() {
    Sender sender;
    pad::MouseStick stick;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool loggedMissing = false;
    while (!stop_) {
        Opened dev = OpenPad();
        if (dev.handle == INVALID_HANDLE_VALUE) {
            if (!loggedMissing) LUMA_INFO("Controller input: no DualSense or DualShock 4 found (USB or Bluetooth)");
            loggedMissing = true;
            { std::lock_guard<std::mutex> lock(mutex_); snap_.connected = false; }
            for (int i = 0; i < 30 && !stop_; ++i) Sleep(100);
            continue;
        }
        loggedMissing = false;
        LUMA_INFO("Controller input: %s connected (%s)", pad::ModelName(dev.model), dev.bluetooth ? "Bluetooth" : "USB");
        if (dev.bluetooth && dev.model == pad::Model::DualSense && dev.featureLength > 0 && dev.featureLength <= 4096) {
            // Asking for the calibration data switches a Bluetooth DualSense from its short report
            // to the full one (buttons, motion, touch and battery).
            std::vector<BYTE> feature(dev.featureLength);
            feature[0] = 0x05;
            HidD_GetFeature(dev.handle, feature.data(), static_cast<ULONG>(feature.size()));
        }
        stats_.Reset();
        stick.Reset();
        { std::lock_guard<std::mutex> lock(mutex_); snap_ = Snapshot(); snap_.connected = true; snap_.bluetooth = dev.bluetooth; }

        std::vector<uint8_t> buf(dev.inputLength);
        OVERLAPPED ov{};
        ov.hEvent = event;
        bool pending = false;
        uint32_t lastButtons = 0;
        uint64_t lastUs = 0;
        pad::State state;
        bool failed = false;
        while (!stop_ && !failed) {
            DWORD got = 0;
            if (!pending) {
                ResetEvent(event);
                if (!ReadFile(dev.handle, buf.data(), static_cast<DWORD>(buf.size()), &got, &ov)) {
                    const DWORD err = GetLastError();
                    if (err != ERROR_IO_PENDING) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        snap_.error = err;
                        failed = true;
                        break;
                    }
                    pending = true;
                }
            }
            if (pending) {
                const DWORD w = WaitForSingleObject(event, 100);
                if (w == WAIT_TIMEOUT) continue;  // the read stays queued
                if (!GetOverlappedResult(dev.handle, &ov, &got, FALSE)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    snap_.error = GetLastError();
                    failed = true;
                    break;
                }
                pending = false;
            }
            if (!pad::ParseReport(dev.model, buf.data(), got, &state)) continue;
            const uint64_t now = NowUs();
            pad::Mapping mapping;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.OnReport(now);
                snap_.state = state;
                snap_.reports++;
                snap_.rateHz = stats_.rateHz();
                snap_.averageMs = stats_.averageMs();
                snap_.maxMs = stats_.maxMs();
                snap_.jitterMs = stats_.jitterMs();
                if (snap_.reports % 8 == 0) snap_.history = stats_.History();  // for the graph, not every report
                mapping = mapping_;
                snap_.mapping = mapping.enabled && mapping.AnyMapped();
            }
            if (mapping.enabled) {
                sender.Apply(pad::Diff(mapping, lastButtons, state.buttons));
                if (mapping.rightStickMouse && lastUs) {
                    int dx, dy;
                    stick.Step(mapping, state.rx, state.ry, static_cast<double>(now - lastUs) / 1000.0, &dx, &dy);
                    sender.Move(dx, dy);
                }
            } else if (lastButtons) {
                sender.ReleaseAll();  // the mapping was switched off with a button held
            }
            lastButtons = state.buttons;
            lastUs = now;
        }
        if (pending) {
            CancelIoEx(dev.handle, &ov);
            DWORD ignored;
            GetOverlappedResult(dev.handle, &ov, &ignored, TRUE);
        }
        sender.ReleaseAll();
        CloseHandle(dev.handle);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const unsigned long err = snap_.error;
            snap_ = Snapshot();
            snap_.error = err;
        }
        if (failed) LUMA_WARN("Controller input: read failed - unplugged or out of range? Looking again.");
        for (int i = 0; i < 20 && !stop_; ++i) Sleep(100);
    }
    CloseHandle(event);
}

}  // namespace luma::app
