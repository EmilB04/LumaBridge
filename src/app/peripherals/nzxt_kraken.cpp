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

// One of the Kraken's HID interfaces (a Kraken can have several): its path and report sizes.
struct Interface {
    std::wstring path;
    DWORD inLen = 0, outLen = 0;
    USHORT usagePage = 0;
};

std::vector<Interface> FindInterfaces(uint16_t pid) {
    std::vector<Interface> found;
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return found;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr)) continue;
        HANDLE q = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof attr;
        if (HidD_GetAttributes(q, &attr) && attr.VendorID == kVid && attr.ProductID == pid) {
            Interface f;
            f.path = detail->DevicePath;
            PHIDP_PREPARSED_DATA pre = nullptr;
            HIDP_CAPS caps{};
            if (HidD_GetPreparsedData(q, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS) {
                f.inLen = caps.InputReportByteLength;
                f.outLen = caps.OutputReportByteLength;
                f.usagePage = caps.UsagePage;
            }
            if (pre) HidD_FreePreparsedData(pre);
            found.push_back(f);
        }
        CloseHandle(q);
    }
    SetupDiDestroyDeviceInfoList(set);
    // The status comes on the vendor interface with 64-byte reports both ways: that one first.
    std::stable_sort(found.begin(), found.end(), [](const Interface& a, const Interface& b) {
        auto score = [](const Interface& f) {
            return (f.inLen >= 64 ? 4 : 0) + (f.outLen >= 64 ? 2 : 0) + (f.usagePage >= 0xFF00 ? 1 : 0);
        };
        return score(a) > score(b);
    });
    return found;
}

}  // namespace

std::vector<Named> FindByName() {
    std::vector<Named> out;
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr)) continue;
        HANDLE q = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof attr;
        wchar_t product[128] = {};
        if (HidD_GetAttributes(q, &attr) && HidD_GetProductString(q, product, sizeof product)) {
            std::wstring lower = product;
            for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
            const bool dup = std::any_of(out.begin(), out.end(), [&](const Named& n) { return n.vid == attr.VendorID && n.pid == attr.ProductID; });
            if (lower.find(L"kraken") != std::wstring::npos && !dup) {
                char name[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, product, -1, name, sizeof name, nullptr, nullptr);
                out.push_back({attr.VendorID, attr.ProductID, name});
            }
        }
        CloseHandle(q);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

void Kraken::Start(uint16_t pid, bool screen) {
    if (thread_.joinable() || !pid) return;
    stop_ = false;
    state_ = KrakenState::Searching;
    thread_ = std::thread(&Kraken::Run, this, pid, screen);
    if (LightingChannels(pid)) lightingThread_ = std::thread(&Kraken::RunLighting, this, pid);
}

void Kraken::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    if (lightingThread_.joinable()) lightingThread_.join();
    lightingActive_ = false;
    state_ = KrakenState::Off;
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = {};
}

void Kraken::SetLighting(const fx::Params& effect, double brightness, bool own) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (effect.kind != effect_.kind || effect.speed != effect_.speed) effectSince_ = GetTickCount64();
    effect_ = effect;
    brightness_ = brightness;
    ownLighting_ = own;
}

void Kraken::RunLighting(uint16_t pid) {
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) { lightingError_ = GetLastError(); return; }
    DWORD outputLength = 0;
    uint64_t nextTry = 0, sentAt = 0;
    Rgb last{};
    auto close = [&] {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
        lightingActive_ = false;
        sentAt = 0;
    };
    while (!stop_) {
        Sleep(100);  // ten colors per second; do not hammer the cooler's USB controller
        fx::Params effect;
        double level;
        bool own;
        uint64_t since;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            effect = effect_; level = brightness_; own = ownLighting_; since = effectSince_;
        }
        if (!own) { close(); nextTry = 0; continue; }
        const uint64_t now = GetTickCount64();
        if (h == INVALID_HANDLE_VALUE) {
            if (now < nextTry) continue;
            nextTry = now + 5000;
            for (const auto& f : FindInterfaces(pid)) {
                if (f.inLen < 64 || f.outLen < 64 || f.outLen > 256 || f.usagePage < 0xFF00) continue;
                h = CreateFileW(f.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                if (h != INVALID_HANDLE_VALUE) { outputLength = f.outLen; break; }
                lightingError_ = GetLastError();
            }
            if (h == INVALID_HANDLE_VALUE) continue;
        }
        const Rgb color = Scale(fx::Render(effect, fx::Seconds(effect, now, since), 0, 1), level);
        if (sentAt && color == last && now - sentAt < 3000) continue;
        const auto packet = FixedLighting(LightingChannels(pid), color);
        std::vector<uint8_t> report(outputLength);
        std::copy(packet.begin(), packet.end(), report.begin());
        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD written = 0;
        const BOOL started = WriteFile(h, report.data(), outputLength, nullptr, &ov);
        const DWORD error = started ? ERROR_SUCCESS : GetLastError();
        bool ok = started || error == ERROR_IO_PENDING;
        if (ok) {
            if (WaitForSingleObject(ev, 500) != WAIT_OBJECT_0) CancelIoEx(h, &ov);
            ok = GetOverlappedResult(h, &ov, &written, TRUE) && written == outputLength;
        }
        if (!ok) {
            lightingError_ = error != ERROR_SUCCESS && error != ERROR_IO_PENDING ? error : GetLastError();
            LUMA_WARN("NZXT Kraken %04X: lighting write failed (error %lu)", pid, lightingError_.load());
            close();
            nextTry = now + 5000;
            continue;
        }
        lightingError_ = 0;
        lightingActive_ = true;
        sentAt = now;
        last = color;
    }
    close();
    CloseHandle(ev);
}

void Kraken::Run(uint16_t pid, bool screen) {
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    uint64_t nextLook = 0;
    bool logged = false;
    int misses = 0;
    size_t which = 0;  // which of its interfaces is being tried
    DWORD inLen = 64, outLen = 64;
    auto nap = [&](int ms) {
        for (int t = 0; t < ms && !stop_; t += 100) Sleep(100);
    };
    auto close = [&] {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    };
    while (!stop_) {
        if (h == INVALID_HANDLE_VALUE) {
            const uint64_t now = GetTickCount64();
            if (now < nextLook) {
                nap(500);
                continue;
            }
            nextLook = now + 5000;
            const std::vector<Interface> all = FindInterfaces(pid);
            if (all.empty()) {
                state_ = KrakenState::Searching;
                if (!logged) LUMA_INFO("NZXT Kraken %04X: no HID interface found", pid);
                logged = true;
                continue;
            }
            if (!logged)
                for (const Interface& f : all)
                    LUMA_INFO("NZXT Kraken %04X: HID interface, usage page %04X, reports in %lu / out %lu bytes", pid, f.usagePage,
                              f.inLen, f.outLen);
            logged = true;
            const Interface& f = all[which % all.size()];
            // To ask for the status (read and write); else, when NZXT CAM has it open and
            // doesn't share writing, to listen to the replies CAM asks for (read only).
            listening_ = false;
            h = CreateFileW(f.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, nullptr);
            const DWORD rwError = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
            if (h == INVALID_HANDLE_VALUE) {
                h = CreateFileW(f.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    listening_ = true;
                    LUMA_INFO("NZXT Kraken: can't ask it (error %lu, NZXT CAM holds it?); listening to its replies instead", rwError);
                }
            }
            if (h == INVALID_HANDLE_VALUE) {
                lastError_ = GetLastError();
                LUMA_WARN("NZXT Kraken: couldn't open it (error %lu, read only: %lu)", rwError, lastError_.load());
                state_ = KrakenState::CantOpen;
                ++which;
                continue;
            }
            lastError_ = 0;
            inLen = std::clamp<DWORD>(f.inLen ? f.inLen : 64, 2, 256);
            outLen = std::clamp<DWORD>(f.outLen ? f.outLen : 64, 2, 256);
            misses = 0;
        }
        // Screen models answer a status request; the X models report on their own.
        if (screen && !listening_) {
            uint8_t req[256] = {0x74, 0x01};
            OVERLAPPED ow{};
            ow.hEvent = ev;
            ResetEvent(ev);
            DWORD done = 0;
            if (WriteFile(h, req, outLen, nullptr, &ow) || GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ev, 500) != WAIT_OBJECT_0) CancelIo(h);
                GetOverlappedResult(h, &ow, &done, TRUE);
            }
        }
        Status got;
        bool gone = false;
        const uint64_t until = GetTickCount64() + 1500;
        while (!got.valid && GetTickCount64() < until && !stop_) {
            uint8_t msg[256] = {};
            OVERLAPPED ov{};
            ov.hEvent = ev;
            ResetEvent(ev);
            DWORD n = 0;
            if (!ReadFile(h, msg, inLen, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
                gone = true;
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
        if (gone) {
            LUMA_INFO("NZXT Kraken: unplugged");
            close();
            state_ = KrakenState::Searching;
        } else if (got.valid) {
            if (state_ != KrakenState::Reading) LUMA_INFO("NZXT Kraken: reading (liquid %.1f C, pump %d RPM)", got.liquidC, got.pumpRpm);
            state_ = KrakenState::Reading;
            misses = 0;
        } else if (++misses >= (listening_ ? 15 : 5)) {
            // No status on this interface: try the next one.
            LUMA_INFO("NZXT Kraken: no status reports on this interface");
            close();
            state_ = KrakenState::NoReply;
            ++which;
            nextLook = 0;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (got.valid || h == INVALID_HANDLE_VALUE) status_ = got;
        }
        nap(1000);
    }
    close();
    CloseHandle(ev);
}

}  // namespace luma::app::nzxt
