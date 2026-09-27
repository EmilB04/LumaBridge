// HID transport for ASUS Aura USB lighting controllers (see aura_usb_protocol.h).
// Opens the controller shared, so Armoury Crate can keep its handle; nothing is exclusive.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "aura_usb_protocol.h"

namespace luma::aurausb {

struct DeviceInfo {
    std::wstring path;
    uint16_t vid = 0, pid = 0;
    uint16_t usagePage = 0;
    std::wstring product;
};

// Vendor-defined HID interfaces (usage page >= 0xFF00) of `vid`/`pid` that take 65-byte
// output reports, i.e. the Aura command interface.
std::vector<DeviceInfo> FindControllers(uint16_t vid, uint16_t pid);

class Device {
public:
    ~Device() { Close(); }
    bool Open(const std::wstring& path);
    void Close();
    bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

    bool Write(const Report& r, DWORD timeoutMs = 1000);
    bool Read(Report* r, DWORD timeoutMs);
    // Sends `req` and waits for a reply whose command byte is `replyCmd` (other traffic,
    // e.g. Armoury Crate's, is skipped).
    bool Transact(const Report& req, uint8_t replyCmd, Report* reply, DWORD timeoutMs = 1000);

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
};

}  // namespace luma::aurausb
