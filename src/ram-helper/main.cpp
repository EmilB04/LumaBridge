// LumaBridge-RAM.exe: lights HyperX / Kingston FURY RGB DDR4 for the LumaBridge app.
//
// The SMBus the RAM's lighting controller sits on is only reachable with administrator
// rights (through the signed PawnIO driver and its SmbusPIIX4 module, AMD chipsets), so this
// small helper runs as a scheduled task (scripts\Install-RamTask.ps1, set up once from the
// app, installed under %ProgramFiles%\LumaBridge) and the app, running as the user, starts it
// without a prompt. They talk through the shared memory in hyperx_ram.h.
//
// Safety:
// - Finding the sticks only reads: "receive byte" from 0x27 and from the SPD chips
//   0x50-0x53, and the memory-type byte of each SPD. Nothing is written until the app asks
//   for colors, the firmware lists Kingston / HyperX memory, and the controller answers.
// - Writes go to one address, 0x27, and only to the registers ram::IsAllowed() admits (the
//   per-LED colors and brightness, direct mode, apply). Never to the SPD chips.
// - Every frame holds the system-wide SMBus lock other tools (Armoury Crate, HWiNFO, ...) use.
// - It stops after repeated write failures, and exits when the app exits or goes quiet.
// Log: %ProgramData%\LumaBridge\ram.log.
#include <windows.h>
#include <sddl.h>

#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "hyperx_ram.h"
#include "smbios.h"

namespace {

using namespace luma::app;

using PawnOpen = HRESULT(WINAPI*)(PHANDLE);
using PawnLoad = HRESULT(WINAPI*)(HANDLE, const UCHAR*, SIZE_T);
using PawnExecute = HRESULT(WINAPI*)(HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
using PawnClose = HRESULT(WINAPI*)(HANDLE);

constexpr ULONG64 kWrite = 0, kRead = 1;
constexpr ULONG64 kProtoByte = 1, kProtoByteData = 2;  // I2C_SMBUS_BYTE, I2C_SMBUS_BYTE_DATA

FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%04u-%02u-%02u %02u:%02u:%02u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/"));
}

struct Pawn {
    HMODULE lib = nullptr;
    PawnOpen open = nullptr;
    PawnLoad load = nullptr;
    PawnExecute execute = nullptr;
    PawnClose close = nullptr;
    HANDLE h = nullptr;
};

// PawnIOLib.dll from PawnIO's own install folder only (never from a folder users can write).
bool LoadPawnLib(Pawn* p) {
    wchar_t pf[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) return false;
    const std::wstring path = std::wstring(pf) + L"\\PawnIO\\PawnIOLib.dll";
    p->lib = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!p->lib) return false;
    p->open = reinterpret_cast<PawnOpen>(reinterpret_cast<void (*)()>(GetProcAddress(p->lib, "pawnio_open")));
    p->load = reinterpret_cast<PawnLoad>(reinterpret_cast<void (*)()>(GetProcAddress(p->lib, "pawnio_load")));
    p->execute = reinterpret_cast<PawnExecute>(reinterpret_cast<void (*)()>(GetProcAddress(p->lib, "pawnio_execute")));
    p->close = reinterpret_cast<PawnClose>(reinterpret_cast<void (*)()>(GetProcAddress(p->lib, "pawnio_close")));
    return p->open && p->load && p->execute && p->close;
}

bool ReadFileBytes(const std::wstring& path, std::vector<UCHAR>* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &read, nullptr);
    CloseHandle(f);
    return ok && read == out->size();
}

class Bus {
public:
    Bus(Pawn& p, HANDLE mutex) : p_(p), mutex_(mutex) {}
    bool Lock() {
        const DWORD r = WaitForSingleObject(mutex_, 2000);
        return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    }
    void Unlock() { ReleaseMutex(mutex_); }
    // Receive byte (no register pointer written); true if the device answered.
    bool Answers(uint8_t addr, int* value = nullptr) { return Read(addr, kProtoByte, 0, value); }
    bool ReadByteData(uint8_t addr, uint8_t reg, int* value) { return Read(addr, kProtoByteData, reg, value); }
    // The only write there is: a register of the RAM's RGB controller, from the allow-list.
    bool WriteController(uint8_t reg, uint8_t value) {
        if (!ram::IsAllowed(reg)) return false;
        const ULONG64 in[5] = {ram::kController, kWrite, reg, kProtoByteData, value};
        SIZE_T got = 0;
        return SUCCEEDED(p_.execute(p_.h, "ioctl_smbus_xfer", in, 5, nullptr, 0, &got));
    }

private:
    bool Read(uint8_t addr, ULONG64 proto, uint8_t reg, int* value) {
        const ULONG64 in[4] = {addr, kRead, reg, proto};
        ULONG64 out[1] = {0};
        SIZE_T got = 0;
        if (FAILED(p_.execute(p_.h, "ioctl_smbus_xfer", in, 4, out, 1, &got))) return false;
        if (value) *value = static_cast<int>(out[0] & 0xFF);
        return true;
    }
    Pawn& p_;
    HANDLE mutex_;
};

bool KingstonMemory() {
    constexpr DWORD kRsmb = 0x52534D42;
    const UINT size = GetSystemFirmwareTable(kRsmb, 0, nullptr, 0);
    if (!size) return false;
    std::vector<uint8_t> raw(size);
    if (GetSystemFirmwareTable(kRsmb, 0, raw.data(), size) != size) return false;
    auto lower = [](std::string x) {
        for (auto& c : x) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return x;
    };
    for (const auto& m : sensors::ParseSmbios(raw).memory) {
        const std::string maker = lower(m.manufacturer), part = lower(m.part);
        // Kingston's JEDEC id (0x0198) shows up as a hex string on some boards.
        if (maker.find("kingston") != std::string::npos || maker.find("hyperx") != std::string::npos ||
            maker.find("0198") != std::string::npos || maker.find("9801") != std::string::npos ||
            part.rfind("hx4", 0) == 0 || part.rfind("kf", 0) == 0)
            return true;
    }
    return false;
}

// The shared memory: SYSTEM and administrators full access, signed-in users read/write,
// medium integrity so the (non-elevated) app can write to it.
ram::Shared* CreateShared(HANDLE* mapping) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x6;;;IU)S:(ML;;NW;;;ME)",
                                                              SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
        return nullptr;
    *mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(ram::Shared), ram::kSharedName);
    const DWORD err = GetLastError();
    LocalFree(sa.lpSecurityDescriptor);
    if (!*mapping) return nullptr;
    if (err == ERROR_ALREADY_EXISTS) {  // another helper is running
        CloseHandle(*mapping);
        *mapping = nullptr;
        return nullptr;
    }
    auto* s = static_cast<ram::Shared*>(MapViewOfFile(*mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ram::Shared)));
    if (s) {
        ZeroMemory(s, sizeof *s);
        s->version = ram::kVersion;
        s->helperPid = GetCurrentProcessId();
        s->magic = ram::kMagic;
    }
    return s;
}

void SetStatus(ram::Shared* s, ram::HelperStatus st) {
    if (s->status != st) Log("status %u", static_cast<unsigned>(st));
    s->status = st;
}

// Waits (answering the app's heartbeat) until the app goes away; used after a fatal problem
// so the app can still read the status.
void WaitForApp(ram::Shared* s) {
    for (int i = 0; i < 600; ++i) {  // at most a minute
        s->helperBeat = GetTickCount64();
        if (s->appBeat && GetTickCount64() - s->appBeat > 10000) return;
        Sleep(100);
    }
}

}  // namespace

int wmain() {
    wchar_t pd[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) {
        const std::wstring dir = std::wstring(pd) + L"\\LumaBridge";
        CreateDirectoryW(dir.c_str(), nullptr);
        g_log = _wfopen((dir + L"\\ram.log").c_str(), L"a");
    }
    Log("RAM helper started (pid %lu)", GetCurrentProcessId());

    HANDLE mapping = nullptr;
    ram::Shared* s = CreateShared(&mapping);
    if (!s) {
        Log("shared memory unavailable or another helper is running (error %lu) - exiting", GetLastError());
        return 1;
    }

    // Wait for the app to connect.
    for (int i = 0; i < 150 && !s->appPid; ++i) Sleep(100);
    if (!s->appPid) {
        Log("the app didn't connect - exiting");
        return 1;
    }
    HANDLE app = OpenProcess(SYNCHRONIZE, FALSE, s->appPid);
    Log("app connected (pid %lu)", s->appPid);

    Pawn p;
    if (!LoadPawnLib(&p)) {
        SetStatus(s, ram::HelperStatus::NoPawnIO);
        WaitForApp(s);
        return 1;
    }
    std::vector<UCHAR> blob;
    if (!ReadFileBytes(ExeDir() + L"\\SmbusPIIX4.bin", &blob)) {
        SetStatus(s, ram::HelperStatus::NoModule);
        WaitForApp(s);
        return 1;
    }
    if (FAILED(p.open(&p.h)) || FAILED(p.load(p.h, blob.data(), blob.size()))) {
        SetStatus(s, ram::HelperStatus::ModuleFailed);
        if (p.h) p.close(p.h);
        WaitForApp(s);
        return 1;
    }
    if (!KingstonMemory()) {
        SetStatus(s, ram::HelperStatus::NotKingston);
        p.close(p.h);
        WaitForApp(s);
        return 1;
    }
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\Access_SMBUS.HTP.Method");
    if (!mutex) mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_SMBUS.HTP.Method");
    if (!mutex) {
        SetStatus(s, ram::HelperStatus::BusBusy);
        p.close(p.h);
        WaitForApp(s);
        return 1;
    }
    Bus bus(p, mutex);

    // Find the controller and the sticks (read only).
    uint8_t sticks = 0;
    bool controller = false;
    if (bus.Lock()) {
        controller = bus.Answers(ram::kController);
        for (int slot = 0; slot < ram::kSlots; ++slot) {
            const uint8_t spd = static_cast<uint8_t>(ram::kSpdFirst + slot);
            int type = 0;
            if (bus.Answers(spd) && bus.ReadByteData(spd, 2, &type) && type == 0x0C) sticks |= 1 << slot;  // DDR4
            Log("SPD 0x%02X: %s", spd, sticks & (1 << slot) ? "DDR4 stick" : "nothing / not DDR4");
        }
        bus.Unlock();
    } else {
        SetStatus(s, ram::HelperStatus::BusBusy);
        WaitForApp(s);
        return 1;
    }
    Log("controller at 0x27: %s, sticks 0x%X", controller ? "answers" : "no answer", sticks);
    s->sticks = sticks;
    if (!controller || !sticks) {
        SetStatus(s, controller ? ram::HelperStatus::NoSticks : ram::HelperStatus::NoController);
        p.close(p.h);
        WaitForApp(s);
        return 1;
    }
    SetStatus(s, ram::HelperStatus::Ready);

    uint32_t lastSeq = 0;
    uint64_t lastWrite = 0;
    int failures = 0;
    while (true) {
        Sleep(30);
        const uint64_t now = GetTickCount64();
        s->helperBeat = now;
        if (app && WaitForSingleObject(app, 0) == WAIT_OBJECT_0) {
            Log("the app exited");
            break;
        }
        if (s->appBeat && now - s->appBeat > 10000) {
            Log("no word from the app for 10 s - exiting");
            break;
        }
        if (!s->own) {
            if (s->status == ram::HelperStatus::Active) SetStatus(s, ram::HelperStatus::Ready);
            lastWrite = 0;
            continue;
        }
        const uint32_t seq = s->seq;
        if (seq == lastSeq && lastWrite && now - lastWrite < 3000) continue;  // refresh now and then
        std::array<luma::Rgb, ram::kMaxLeds> colors{};
        for (int i = 0; i < ram::kMaxLeds; ++i)
            colors[static_cast<size_t>(i)] = luma::Rgb{s->colors[i][0], s->colors[i][1], s->colors[i][2]};
        const auto writes = ram::Frame(sticks, colors);
        if (!bus.Lock()) {
            SetStatus(s, ram::HelperStatus::BusBusy);
            continue;
        }
        bool ok = true;
        for (const auto& w : writes) ok = bus.WriteController(w.reg, w.value) && ok;
        bus.Unlock();
        lastSeq = seq;
        lastWrite = now;
        if (ok) {
            failures = 0;
            SetStatus(s, ram::HelperStatus::Active);
        } else if (++failures >= 5) {
            Log("writes keep failing - stopping");
            SetStatus(s, ram::HelperStatus::WriteFailed);
            WaitForApp(s);
            break;
        }
    }
    p.close(p.h);
    CloseHandle(mutex);
    UnmapViewOfFile(s);
    CloseHandle(mapping);
    Log("RAM helper exiting");
    return 0;
}
