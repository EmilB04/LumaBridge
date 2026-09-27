// READ-ONLY probe of the SMBus where RAM lighting controllers live (AMD chipsets, via the
// PawnIO driver's signed SmbusPIIX4 module). First step towards RAM lighting: find out which
// devices answer, without writing anything.
//
//   ram-probe [path\to\SmbusPIIX4.bin]
//
// Requirements: the PawnIO driver installed (https://pawnio.eu), run as administrator, and
// SmbusPIIX4.bin (from PawnIO.Modules releases) next to this exe or passed as an argument.
//
// Safety: only SMBus "receive byte" on a short list of addresses (no register pointer is
// written), plus "read byte data" of register 2 on the SPD EEPROMs (0x50-0x57), which is
// how every DDR4 detector identifies a stick. No page switches, no writes of any kind.
// Every transfer holds the system-wide "Access_SMBUS.HTP.Method" mutex that other hardware
// tools (Armoury Crate, HWiNFO, ...) use to share the bus.
#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using PawnOpen = HRESULT(WINAPI*)(PHANDLE);
using PawnLoad = HRESULT(WINAPI*)(HANDLE, const UCHAR*, SIZE_T);
using PawnExecute = HRESULT(WINAPI*)(HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
using PawnClose = HRESULT(WINAPI*)(HANDLE);

constexpr ULONG64 kRead = 1;
constexpr ULONG64 kProtoByte = 1;      // I2C_SMBUS_BYTE: receive byte (no command written)
constexpr ULONG64 kProtoByteData = 2;  // I2C_SMBUS_BYTE_DATA

struct Pawn {
    HMODULE lib = nullptr;
    PawnOpen open = nullptr;
    PawnLoad load = nullptr;
    PawnExecute execute = nullptr;
    PawnClose close = nullptr;
    HANDLE h = nullptr;
};

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/"));
}

bool LoadPawnLib(Pawn* p) {
    wchar_t pf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    const std::wstring candidates[] = {ExeDir() + L"\\PawnIOLib.dll", std::wstring(pf) + L"\\PawnIO\\PawnIOLib.dll",
                                       L"PawnIOLib.dll"};
    for (const auto& c : candidates) {
        p->lib = LoadLibraryExW(c.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (p->lib) {
            std::wprintf(L"PawnIOLib: %ls\n", c.c_str());
            break;
        }
    }
    if (!p->lib) return false;
    p->open = reinterpret_cast<PawnOpen>(reinterpret_cast<void*>(GetProcAddress(p->lib, "pawnio_open")));
    p->load = reinterpret_cast<PawnLoad>(reinterpret_cast<void*>(GetProcAddress(p->lib, "pawnio_load")));
    p->execute = reinterpret_cast<PawnExecute>(reinterpret_cast<void*>(GetProcAddress(p->lib, "pawnio_execute")));
    p->close = reinterpret_cast<PawnClose>(reinterpret_cast<void*>(GetProcAddress(p->lib, "pawnio_close")));
    return p->open && p->load && p->execute && p->close;
}

bool ReadFileBytes(const std::wstring& path, std::vector<UCHAR>* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &read, nullptr);
    CloseHandle(f);
    return ok && read == out->size();
}

// Holds the shared SMBus mutex for one transfer.
class BusLock {
public:
    explicit BusLock(HANDLE m) : m_(m) {
        DWORD r = m_ ? WaitForSingleObject(m_, 2000) : WAIT_FAILED;
        held_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    }
    ~BusLock() {
        if (held_) ReleaseMutex(m_);
    }
    bool held() const { return held_; }

private:
    HANDLE m_;
    bool held_ = false;
};

// Returns true and the byte if the device at `addr` answered.
bool SmbusRead(Pawn& p, HANDLE mutex, ULONG64 addr, ULONG64 protocol, ULONG64 command, int* value, HRESULT* hr) {
    BusLock lock(mutex);
    if (!lock.held()) {
        *hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        return false;
    }
    const ULONG64 in[4] = {addr, kRead, command, protocol};
    ULONG64 out[1] = {0};
    SIZE_T got = 0;
    *hr = p.execute(p.h, "ioctl_smbus_xfer", in, 4, out, 1, &got);
    if (FAILED(*hr)) return false;
    *value = static_cast<int>(out[0] & 0xFF);
    return true;
}

const wchar_t* Describe(int addr) {
    if (addr >= 0x18 && addr <= 0x1F) return L"DDR4 temperature sensor (TSOD)";
    if (addr == 0x27) return L"HyperX DDR4 RGB controller (expected address)";
    if (addr >= 0x50 && addr <= 0x57) return L"SPD EEPROM (RAM slot)";
    if (addr >= 0x58 && addr <= 0x5F) return L"Kingston FURY / other RGB controllers";
    if (addr >= 0x60 && addr <= 0x67) return L"RGB controllers (DDR5-era addresses)";
    if (addr >= 0x70 && addr <= 0x77) return L"ENE / ASUS Aura DRAM controllers";
    return L"";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wprintf(L"LumaBridge RAM probe (read-only)\n\n");
    Pawn p;
    if (!LoadPawnLib(&p)) {
        std::wprintf(L"PawnIO is not installed (PawnIOLib.dll not found).\n"
                     L"Install it from https://pawnio.eu and run this again.\n");
        return 1;
    }
    // Next to this exe, else the copy bundled with LumaBridge (..\pawnio\modules).
    std::wstring module = argc > 1 ? argv[1] : ExeDir() + L"\\SmbusPIIX4.bin";
    if (argc <= 1 && GetFileAttributesW(module.c_str()) == INVALID_FILE_ATTRIBUTES)
        module = ExeDir() + L"\\..\\pawnio\\modules\\SmbusPIIX4.bin";
    std::vector<UCHAR> blob;
    if (!ReadFileBytes(module, &blob)) {
        std::wprintf(L"Module not found: %ls\n"
                     L"Download the PawnIO.Modules release and put SmbusPIIX4.bin next to ram-probe.exe.\n",
                     module.c_str());
        return 1;
    }
    HRESULT hr = p.open(&p.h);
    if (FAILED(hr)) {
        std::wprintf(L"pawnio_open failed (0x%08lX). Run as administrator.\n", static_cast<unsigned long>(hr));
        return 1;
    }
    hr = p.load(p.h, blob.data(), blob.size());
    if (FAILED(hr)) {
        std::wprintf(L"Loading SmbusPIIX4.bin failed (0x%08lX): not an AMD chipset, or an unsigned/old module.\n",
                     static_cast<unsigned long>(hr));
        p.close(p.h);
        return 1;
    }

    ULONG64 id[3] = {};
    SIZE_T got = 0;
    hr = p.execute(p.h, "ioctl_identity", nullptr, 0, id, 3, &got);
    if (SUCCEEDED(hr)) {
        char type[6] = {};
        for (int i = 0; i < 5; ++i) type[i] = static_cast<char>((id[0] >> (8 * i)) & 0xFF);
        std::wprintf(L"SMBus controller: %hs, I/O base 0x%04llX, PCI %04llX:%04llX (subsystem %04llX:%04llX)\n", type,
                     id[1], id[2] & 0xFFFF, (id[2] >> 16) & 0xFFFF, (id[2] >> 32) & 0xFFFF, (id[2] >> 48) & 0xFFFF);
    } else {
        std::wprintf(L"ioctl_identity failed (0x%08lX)\n", static_cast<unsigned long>(hr));
    }

    // Same name every SMBus-aware tool uses; "Global\" makes it visible across sessions.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\Access_SMBUS.HTP.Method");
    if (!mutex) mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_SMBUS.HTP.Method");
    std::wprintf(L"Shared SMBus lock: %ls\n\n", mutex ? L"ok" : L"not available (aborting)");
    if (!mutex) {
        p.close(p.h);
        return 1;
    }

    std::vector<int> addrs;
    for (int a = 0x18; a <= 0x1F; ++a) addrs.push_back(a);
    addrs.push_back(0x27);
    for (int a = 0x50; a <= 0x77; ++a)
        if (a < 0x68 || a >= 0x70) addrs.push_back(a);  // skip 0x68-0x6F (clock generators, RTCs)

    std::wprintf(L"Addresses that answer (receive byte, read-only):\n");
    int found = 0;
    for (int a : addrs) {
        int v = 0;
        HRESULT r;
        if (!SmbusRead(p, mutex, static_cast<ULONG64>(a), kProtoByte, 0, &v, &r)) continue;
        ++found;
        std::wprintf(L"  0x%02X  answers (0x%02X)  %ls", a, v, Describe(a));
        if (a >= 0x50 && a <= 0x57) {
            int type = 0;
            if (SmbusRead(p, mutex, static_cast<ULONG64>(a), kProtoByteData, 2, &type, &r))
                std::wprintf(L"  [memory type byte 0x%02X%ls]", type, type == 0x0C ? L" = DDR4" : type == 0x12 ? L" = DDR5" : L"");
        }
        std::wprintf(L"\n");
        Sleep(2);
    }
    if (!found) std::wprintf(L"  (none)\n");

    CloseHandle(mutex);
    p.close(p.h);
    std::wprintf(L"\nDone. Nothing was written to the bus.\n");
    return 0;
}
