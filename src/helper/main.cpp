// LumaBridge-Helper.exe: the hardware access the LumaBridge app can't do as a normal user.
//
//   - RAM lighting: HyperX / Kingston FURY RGB DDR4 on the SMBus (hyperx_ram.h);
//   - sensors: the Ryzen CPU temperature and the Nuvoton monitoring chip's fans and board
//     temperatures (hw_sensors.h), instead of LibreHardwareMonitor.
//
// It talks to the signed PawnIO driver directly (device I/O control; no PawnIO library),
// loading one small signed PawnIO module per job from its own folder. It runs as a
// scheduled task as SYSTEM (scripts\Install-Helper.ps1, set up once from the app, installed
// under %ProgramFiles%\LumaBridge), and the app starts it without a prompt. They talk through
// the shared memory in helper_ipc.h.
//
// Safety:
// - Sensors are only read. The monitoring chip is put into configuration mode to find it
//   (and its I/O space lock is cleared, as the Linux driver does), then only its registers
//   are read; the CPU temperature is one register read.
// - RAM: finding the sticks only reads, and nothing is written until the app asks for RAM
//   lighting, the firmware lists Kingston / HyperX memory and the controller answers. Writes
//   go to one SMBus address, 0x27, and only to the registers ram::IsAllowed() admits.
// - Every access holds the system-wide lock other tools use for that bus
//   (Access_SMBUS / Access_ISABUS / Access_PCI).
// - It exits when the app exits or goes quiet.
// Log: %ProgramData%\LumaBridge\helper.log.
#include <windows.h>
#include <sddl.h>

#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "helper_ipc.h"
#include "hw_sensors.h"
#include "hyperx_ram.h"
#include "smbios.h"

namespace {

using namespace luma::app;

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

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>* out) {
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

// ---- PawnIO, through its device I/O control interface --------------------------------

constexpr DWORD kPawnDeviceType = 41394u << 16;
constexpr DWORD kIoctlLoad = kPawnDeviceType | (0x821u << 2);
constexpr DWORD kIoctlExecute = kPawnDeviceType | (0x841u << 2);
constexpr size_t kFnNameLength = 32;

bool PawnIoPresent() {
    HANDLE h = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// One PawnIO handle with one module loaded.
class PawnModule {
public:
    enum class Result { Ok, NoDriver, NoFile, Refused };
    ~PawnModule() { Close(); }
    Result Load(const wchar_t* file) {
        std::vector<uint8_t> blob;
        if (!ReadFileBytes(ExeDir() + L"\\" + file, &blob)) return Result::NoFile;
        h_ = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return Result::NoDriver;
        DWORD ret = 0;
        if (!DeviceIoControl(h_, kIoctlLoad, blob.data(), static_cast<DWORD>(blob.size()), nullptr, 0, &ret, nullptr)) {
            Log("PawnIO refused %ls (error %lu)", file, GetLastError());
            Close();
            return Result::Refused;
        }
        return Result::Ok;
    }
    bool loaded() const { return h_ != INVALID_HANDLE_VALUE; }
    bool Execute(const char* fn, std::initializer_list<uint64_t> in, uint64_t* out = nullptr, size_t outCount = 0) {
        if (!loaded()) return false;
        std::vector<uint8_t> buf(kFnNameLength + in.size() * sizeof(uint64_t), 0);
        strncpy(reinterpret_cast<char*>(buf.data()), fn, kFnNameLength - 1);
        size_t at = kFnNameLength;
        for (uint64_t v : in) {
            memcpy(buf.data() + at, &v, sizeof v);
            at += sizeof v;
        }
        std::vector<uint64_t> outBuf(outCount ? outCount : 1);
        DWORD ret = 0;
        if (!DeviceIoControl(h_, kIoctlExecute, buf.data(), static_cast<DWORD>(buf.size()), outBuf.data(),
                             static_cast<DWORD>(outCount * sizeof(uint64_t)), &ret, nullptr))
            return false;
        for (size_t i = 0; i < outCount; ++i) out[i] = outBuf[i];
        return true;
    }
    void Close() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

// A system-wide lock other hardware tools share ("Global\Access_SMBUS.HTP.Method", ...).
class NamedLock {
public:
    explicit NamedLock(const wchar_t* name) {
        m_ = CreateMutexW(nullptr, FALSE, name);
        if (!m_) m_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
    }
    ~NamedLock() {
        if (m_) CloseHandle(m_);
    }
    bool Lock(DWORD ms = 2000) {
        if (!m_) return false;
        const DWORD r = WaitForSingleObject(m_, ms);
        return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    }
    void Unlock() { ReleaseMutex(m_); }

private:
    HANDLE m_ = nullptr;
};

// ---- RAM (SMBus) -----------------------------------------------------------------------

constexpr uint64_t kSmbusWrite = 0, kSmbusRead = 1, kProtoByte = 1, kProtoByteData = 2;

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

class Ram {
public:
    helper::RamStatus status() const { return status_; }
    uint8_t sticks() const { return sticks_; }

    // Finds the controller and the sticks (read only).
    void Start() {
        status_ = helper::RamStatus::Starting;
        switch (smbus_.Load(L"SmbusPIIX4.bin")) {
        case PawnModule::Result::Ok: break;
        case PawnModule::Result::NoFile: status_ = helper::RamStatus::NoModule; return;
        default: status_ = helper::RamStatus::ModuleFailed; return;
        }
        if (!KingstonMemory()) {
            status_ = helper::RamStatus::NotKingston;
            return;
        }
        if (!lock_.Lock()) {
            status_ = helper::RamStatus::BusBusy;
            return;
        }
        const bool controller = Answers(ram::kController);
        for (int slot = 0; slot < ram::kSlots; ++slot) {
            const uint8_t spd = static_cast<uint8_t>(ram::kSpdFirst + slot);
            int type = 0;
            if (Answers(spd) && ReadByteData(spd, 2, &type) && type == 0x0C) sticks_ |= 1 << slot;  // DDR4
        }
        lock_.Unlock();
        Log("RAM: controller at 0x27 %s, DDR4 sticks 0x%X", controller ? "answers" : "doesn't answer", sticks_);
        status_ = !controller ? helper::RamStatus::NoController
                  : !sticks_  ? helper::RamStatus::NoSticks
                              : helper::RamStatus::Ready;
    }

    // Shows the colors in `sh` if they changed (or now and then, in case something else
    // lit the sticks); lets go of the sticks when the app stops asking.
    void Update(const helper::Shared* sh, uint64_t now) {
        if (status_ != helper::RamStatus::Ready && status_ != helper::RamStatus::Active &&
            status_ != helper::RamStatus::BusBusy)
            return;
        release_ = static_cast<helper::RamRelease>(sh->ramRelease);
        if (!sh->ramOwn) {
            LetGo();
            if (status_ == helper::RamStatus::Active) status_ = helper::RamStatus::Ready;
            lastWrite_ = 0;
            return;
        }
        const uint32_t seq = sh->ramSeq;
        if (seq == lastSeq_ && lastWrite_ && now - lastWrite_ < 3000) return;
        std::array<luma::Rgb, ram::kMaxLeds> colors{};
        for (int i = 0; i < ram::kMaxLeds; ++i)
            colors[static_cast<size_t>(i)] = luma::Rgb{sh->ramColors[i][0], sh->ramColors[i][1], sh->ramColors[i][2]};
        const auto writes = ram::Frame(sticks_, colors);
        if (!lock_.Lock()) {
            status_ = helper::RamStatus::BusBusy;
            return;
        }
        bool ok = true;
        for (const auto& w : writes) ok = Write(w.reg, w.value) && ok;
        lock_.Unlock();
        lastSeq_ = seq;
        lastWrite_ = now;
        if (ok) {
            failures_ = 0;
            owning_ = true;
            status_ = helper::RamStatus::Active;
        } else if (++failures_ >= 5) {
            Log("RAM: writes keep failing - stopped");
            status_ = helper::RamStatus::WriteFailed;
        }
    }

    // Hands the sticks back (as the app chose) if LumaBridge was lighting them: their own
    // rainbow, off, or the last color. Without this they'd stay frozen on the last color.
    void LetGo() {
        if (!owning_) return;
        owning_ = false;
        if (release_ == helper::RamRelease::KeepLast) return;
        std::vector<ram::Write> writes;
        if (release_ == helper::RamRelease::Off) writes = ram::Frame(sticks_, std::array<luma::Rgb, ram::kMaxLeds>{});
        else writes = ram::OwnRainbow();
        if (!lock_.Lock()) return;
        for (const auto& w : writes) Write(w.reg, w.value);
        lock_.Unlock();
        Log("RAM: let go (%s)", release_ == helper::RamRelease::Off ? "off" : "the sticks' own rainbow");
    }

private:
    bool Read(uint8_t addr, uint64_t proto, uint8_t reg, int* value) {
        uint64_t out[1] = {0};
        if (!smbus_.Execute("ioctl_smbus_xfer", {addr, kSmbusRead, reg, proto}, out, 1)) return false;
        if (value) *value = static_cast<int>(out[0] & 0xFF);
        return true;
    }
    bool Answers(uint8_t addr) { return Read(addr, kProtoByte, 0, nullptr); }
    bool ReadByteData(uint8_t addr, uint8_t reg, int* value) { return Read(addr, kProtoByteData, reg, value); }
    // The only write there is: a register of the RAM's RGB controller, from the allow-list.
    bool Write(uint8_t reg, uint8_t value) {
        if (!ram::IsAllowed(reg)) return false;
        return smbus_.Execute("ioctl_smbus_xfer", {ram::kController, kSmbusWrite, reg, kProtoByteData, value});
    }

    PawnModule smbus_;
    NamedLock lock_{L"Global\\Access_SMBUS.HTP.Method"};
    helper::RamStatus status_ = helper::RamStatus::Off;
    uint8_t sticks_ = 0;
    uint32_t lastSeq_ = 0;
    uint64_t lastWrite_ = 0;
    int failures_ = 0;
    bool owning_ = false;
    helper::RamRelease release_ = helper::RamRelease::OwnRainbow;
};

// ---- Sensors ------------------------------------------------------------------------------

class Sensors {
public:
    void Start() {
        if (cpu_.Load(L"AMDFamily17.bin") == PawnModule::Result::Ok) {
            uint64_t v = 0;
            haveCpu_ = pci_.Lock() && cpu_.Execute("ioctl_read_smn", {hw::kTctlSmn}, &v, 1);
            if (haveCpu_) pci_.Unlock();
            Log("CPU temperature: %s", haveCpu_ ? "Ryzen Tctl" : "not available");
        } else {
            Log("CPU temperature: AMDFamily17 module didn't load (not an AMD Ryzen?)");
        }
        if (lpc_.Load(L"LpcIO.bin") == PawnModule::Result::Ok) FindChip();
        else Log("monitoring chip: LpcIO module didn't load");
    }

    const char* chip() const { return chip_; }

    // Reads everything into `out`; returns the count.
    int Read(helper::SensorEntry* out, int max) {
        int n = 0;
        auto add = [&](helper::SensorKind kind, helper::SensorType type, double value, const char* name) {
            if (n >= max) return;
            out[n].kind = kind;
            out[n].type = type;
            out[n].value = value;
            snprintf(out[n].name, sizeof out[n].name, "%s", name);
            ++n;
        };
        if (haveCpu_ && pci_.Lock()) {
            uint64_t v = 0;
            const bool ok = cpu_.Execute("ioctl_read_smn", {hw::kTctlSmn}, &v, 1);
            pci_.Unlock();
            if (ok) add(helper::SensorKind::Cpu, helper::SensorType::Temperature, hw::TctlCelsius(static_cast<uint32_t>(v)),
                        "Core (Tctl/Tdie)");
        }
        if (base_ && isa_.Lock()) {
            for (const auto& t : hw::kTemps) {
                double c;
                if (TemperatureValid(ReadChip(t.reg), &c)) add(helper::SensorKind::Board, helper::SensorType::Temperature, c, t.name);
            }
            for (const auto& fan : hw::kFans) {
                const uint8_t high = ReadChip(fan.reg), low = ReadChip(static_cast<uint16_t>(fan.reg + 1));
                add(helper::SensorKind::Board, helper::SensorType::Fan, hw::FanRpm(high, low), fan.name);
            }
            SelectBank(0);
            isa_.Unlock();
        }
        return n;
    }

private:
    static bool TemperatureValid(uint8_t raw, double* c) { return hw::TemperatureValid(raw, c); }

    uint8_t SioIn(uint8_t reg) {
        uint64_t v = 0;
        lpc_.Execute("ioctl_superio_inb", {reg}, &v, 1);
        return static_cast<uint8_t>(v);
    }
    uint16_t SioInWord(uint8_t reg) {
        uint64_t v = 0;
        lpc_.Execute("ioctl_superio_inw", {reg}, &v, 1);
        return static_cast<uint16_t>(v);
    }
    void SioOut(uint8_t reg, uint8_t value) { lpc_.Execute("ioctl_superio_outb", {reg, value}); }
    void PortOut(uint16_t port, uint8_t value) { lpc_.Execute("ioctl_pio_outb", {port, value}); }
    uint8_t PortIn(uint16_t port) {
        uint64_t v = 0;
        lpc_.Execute("ioctl_pio_inb", {port}, &v, 1);
        return static_cast<uint8_t>(v);
    }
    void SelectBank(uint8_t bank) {
        PortOut(static_cast<uint16_t>(base_ + hw::kAddressOffset), hw::kBankSelect);
        PortOut(static_cast<uint16_t>(base_ + hw::kDataOffset), bank);
    }
    uint8_t ReadChip(uint16_t reg) {
        SelectBank(static_cast<uint8_t>(reg >> 8));
        PortOut(static_cast<uint16_t>(base_ + hw::kAddressOffset), static_cast<uint8_t>(reg & 0xFF));
        return PortIn(static_cast<uint16_t>(base_ + hw::kDataOffset));
    }

    void FindChip() {
        if (!isa_.Lock()) {
            Log("monitoring chip: the ISA bus lock is busy");
            return;
        }
        for (uint64_t slot = 0; slot < 2 && !base_; ++slot) {
            const uint16_t port = slot == 0 ? 0x2E : 0x4E;
            if (!lpc_.Execute("ioctl_select_slot", {slot})) continue;
            PortOut(port, hw::kSuperIoEnter);
            PortOut(port, hw::kSuperIoEnter);
            const uint8_t id = SioIn(hw::kChipIdRegister), rev = SioIn(hw::kChipRevisionRegister);
            const char* name = hw::NuvotonChip(id, rev);
            if (!name) {
                if (id != 0 && id != 0xFF) Log("monitoring chip at 0x%X: id %02X%02X (not supported yet)", port, id, rev);
                PortOut(port, hw::kSuperIoExit);
                continue;
            }
            lpc_.Execute("ioctl_find_bars", {});
            SioOut(hw::kLogicalDeviceRegister, hw::kHardwareMonitorLdn);
            const uint16_t address = SioInWord(hw::kBaseAddressRegister);
            Sleep(1);
            const uint16_t verify = SioInWord(hw::kBaseAddressRegister);
            // The hardware-monitor I/O space lock (bit 4 of 0x28) blocks reads; clear it, as the
            // Linux driver does.
            const uint8_t options = SioIn(0x28);
            if (address == verify && (options & 0x10)) SioOut(0x28, static_cast<uint8_t>(options & ~0x10));
            PortOut(port, hw::kSuperIoExit);
            if (address != verify || address < 0x100 || (address & 0x07) != 0) {
                Log("monitoring chip %s: bad address %04X / %04X", name, address, verify);
                continue;
            }
            base_ = address;
            const uint16_t vendor = static_cast<uint16_t>(ReadChip(hw::kVendorHigh) << 8 | ReadChip(hw::kVendorLow));
            SelectBank(0);
            if (vendor != hw::kNuvotonVendor) {
                Log("monitoring chip %s at %04X: vendor %04X, not Nuvoton - ignored", name, address, vendor);
                base_ = 0;
                continue;
            }
            snprintf(chip_, sizeof chip_, "%s", name);
            Log("monitoring chip: Nuvoton %s at 0x%04X", name, address);
        }
        isa_.Unlock();
    }

    PawnModule cpu_, lpc_;
    NamedLock pci_{L"Global\\Access_PCI"};
    NamedLock isa_{L"Global\\Access_ISABUS.HTP.Method"};
    bool haveCpu_ = false;
    uint16_t base_ = 0;
    char chip_[32] = "";
};

// ---- Shared memory -------------------------------------------------------------------------

// SYSTEM and administrators full access, signed-in users read/write, medium integrity so the
// (non-elevated) app can write to it.
helper::Shared* CreateShared(HANDLE* mapping) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x6;;;IU)S:(ML;;NW;;;ME)",
                                                              SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
        return nullptr;
    *mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(helper::Shared), helper::kSharedName);
    const DWORD err = GetLastError();
    LocalFree(sa.lpSecurityDescriptor);
    if (!*mapping) return nullptr;
    if (err == ERROR_ALREADY_EXISTS) {  // another helper is running
        CloseHandle(*mapping);
        *mapping = nullptr;
        return nullptr;
    }
    auto* s = static_cast<helper::Shared*>(MapViewOfFile(*mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(helper::Shared)));
    if (s) {
        ZeroMemory(s, sizeof *s);
        s->version = helper::kVersion;
        s->helperPid = GetCurrentProcessId();
        MemoryBarrier();
        s->magic = helper::kMagic;
    }
    return s;
}

}  // namespace

int wmain() {
    wchar_t pd[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) {
        const std::wstring dir = std::wstring(pd) + L"\\LumaBridge";
        CreateDirectoryW(dir.c_str(), nullptr);
        g_log = _wfopen((dir + L"\\helper.log").c_str(), L"a");
    }
    Log("hardware helper started (pid %lu)", GetCurrentProcessId());

    HANDLE mapping = nullptr;
    helper::Shared* s = CreateShared(&mapping);
    if (!s) {
        Log("shared memory unavailable or another helper is running (error %lu) - exiting", GetLastError());
        return 1;
    }
    for (int i = 0; i < 150 && !s->appPid; ++i) Sleep(100);  // wait for the app to connect
    if (!s->appPid) {
        Log("the app didn't connect - exiting");
        return 1;
    }
    HANDLE app = OpenProcess(SYNCHRONIZE, FALSE, s->appPid);
    Log("app connected (pid %lu)", s->appPid);

    Sensors sensors;
    Ram ram;
    if (!PawnIoPresent()) {
        Log("the PawnIO driver isn't installed");
        s->status = helper::Status::NoPawnIO;
    } else {
        sensors.Start();
        snprintf(s->chip, sizeof s->chip, "%s", sensors.chip());
        s->status = helper::Status::Running;
    }

    uint64_t nextSensors = 0, appBeat = s->appBeat, appBeatAt = GetTickCount64();
    helper::RamStatus lastRam = helper::RamStatus::Off;
    while (true) {
        Sleep(30);
        const uint64_t now = GetTickCount64();
        s->helperBeat++;
        if (app && WaitForSingleObject(app, 0) == WAIT_OBJECT_0) {
            Log("the app exited");
            break;
        }
        // The app's heartbeat, by change (the two processes' clocks aren't compared).
        if (s->appBeat != appBeat) {
            appBeat = s->appBeat;
            appBeatAt = now;
        } else if (now - appBeatAt > 10000) {
            Log("no word from the app for 10 s - exiting");
            break;
        }
        if (s->status != helper::Status::Running) continue;

        if (s->ramWanted && ram.status() == helper::RamStatus::Off) ram.Start();
        ram.Update(s, now);
        s->ram = ram.status();
        s->sticks = ram.sticks();
        if (ram.status() != lastRam) {
            Log("RAM status %u", static_cast<unsigned>(ram.status()));
            lastRam = ram.status();
        }

        if (now >= nextSensors) {
            nextSensors = now + 1000;
            helper::SensorEntry read[helper::kMaxSensors];
            const int n = sensors.Read(read, helper::kMaxSensors);
            s->sensorSeq++;  // odd: being written
            MemoryBarrier();
            for (int i = 0; i < n; ++i) s->sensors[i] = read[i];
            s->sensorCount = static_cast<uint32_t>(n);
            MemoryBarrier();
            s->sensorSeq++;
        }
    }
    ram.LetGo();  // LumaBridge is gone: hand the sticks back
    UnmapViewOfFile(s);
    CloseHandle(mapping);
    Log("hardware helper exiting");
    return 0;
}
