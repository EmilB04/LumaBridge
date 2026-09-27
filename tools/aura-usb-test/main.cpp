// Talks to the motherboard's ASUS Aura USB controller directly (no Armoury Crate / Aura SDK).
// A probe to confirm the protocol on real hardware before LumaBridge relies on it.
//
//   aura-usb-test info                        firmware + configuration table
//   aura-usb-test scan [seconds]              light channels 0..7 one at a time, in red
//   aura-usb-test color RRGGBB [channel|all]  set a static color (default: all channels)
//
// Options: --pid 1939 (hex USB product id; default 1939), --leds 120 (LEDs per channel).
// Nothing is saved to the controller: reopening Armoury Crate or rebooting restores it.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "aura_usb.h"

using namespace luma;
using namespace luma::aurausb;

namespace {

constexpr int kMaxChannel = 7;

int Usage() {
    std::fwprintf(stderr,
                  L"usage:\n"
                  L"  aura-usb-test info\n"
                  L"  aura-usb-test scan [seconds-per-channel]\n"
                  L"  aura-usb-test color RRGGBB [channel|all]\n"
                  L"options: --pid <hex, default 1939>  --leds <per channel, default 120>\n");
    return 2;
}

bool ParseHex(const wchar_t* s, Rgb* out) {
    if (*s == L'#') ++s;
    wchar_t* end = nullptr;
    unsigned long v = std::wcstoul(s, &end, 16);
    if (end == s || *end != L'\0' || std::wcslen(s) != 6) return false;
    *out = Rgb{static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    return true;
}

// Effect channels and direct channels are numbered differently (see aura_usb_protocol.h),
// so every effect channel is switched to direct mode up front.
bool AllDirect(Device& dev) {
    bool ok = true;
    for (int ch = 0; ch <= kMaxChannel; ++ch) ok &= dev.Write(SetModeRequest(static_cast<uint8_t>(ch), kModeDirect));
    return ok;
}

bool SetChannel(Device& dev, int channel, Rgb color, int leds) {
    std::vector<Rgb> frame(static_cast<size_t>(leds), color);
    for (const Report& r : DirectColorRequests(static_cast<uint8_t>(channel), frame))
        if (!dev.Write(r)) return false;
    return true;
}

void PrintHex(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) std::wprintf(L"%02X%ls", p[i], (i % 16 == 15 || i + 1 == n) ? L"\n    " : L" ");
    std::wprintf(L"\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    uint16_t pid = 0x1939;
    int leds = 120;
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--pid") && i + 1 < argc) pid = static_cast<uint16_t>(std::wcstoul(argv[++i], nullptr, 16));
        else if (!wcscmp(argv[i], L"--leds") && i + 1 < argc) leds = _wtoi(argv[++i]);
        else args.push_back(argv[i]);
    }
    if (args.empty()) return Usage();
    if (leds < 1 || leds > 240) leds = 120;

    auto found = FindControllers(kVendorAsus, pid);
    if (found.empty()) {
        std::fwprintf(stderr, L"No Aura USB controller 0B05:%04X with a vendor command interface found.\n", pid);
        return 1;
    }
    std::wprintf(L"Found %d interface(s):\n", static_cast<int>(found.size()));
    for (const auto& d : found)
        std::wprintf(L"  %ls  usage page %04X\n    %ls\n", d.product.c_str(), d.usagePage, d.path.c_str());

    Device dev;
    if (!dev.Open(found[0].path)) {
        std::fwprintf(stderr, L"Could not open the controller (error %lu).\n", GetLastError());
        return 1;
    }

    Report reply;
    std::string fw;
    if (dev.Transact(FirmwareRequest(), 0x02, &reply) && ParseFirmware(reply, &fw))
        std::wprintf(L"Firmware: %hs\n", fw.c_str());
    else
        std::wprintf(L"Firmware: (no answer)\n");

    ConfigTable cfg;
    const bool haveCfg = dev.Transact(ConfigRequest(), 0x30, &reply) && ParseConfig(reply, &cfg);
    if (haveCfg)
        std::wprintf(L"Config: %d ARGB header(s), %d mainboard LED(s), %d RGB header(s)\n", cfg.ArgbHeaders(),
                     cfg.MainboardLeds(), cfg.RgbHeaders());
    else
        std::wprintf(L"Config: (no answer)\n");

    const std::wstring& cmd = args[0];
    if (cmd == L"info") {
        if (haveCfg) {
            std::wprintf(L"Raw config table:\n    ");
            PrintHex(cfg.raw.data(), cfg.raw.size());
        }
        return 0;
    }

    if (cmd == L"scan") {
        const int secs = args.size() > 1 ? _wtoi(args[1].c_str()) : 4;
        std::wprintf(L"Switching all effect channels to direct mode: %ls\n", AllDirect(dev) ? L"ok" : L"write failed");
        for (int ch = 0; ch <= kMaxChannel; ++ch) SetChannel(dev, ch, Rgb{}, leds);  // start dark
        std::wprintf(L"Lighting direct channels 0..%d in RED, one at a time. Note what lights up each time.\n",
                     kMaxChannel);
        for (int ch = 0; ch <= kMaxChannel; ++ch) {
            std::wprintf(L"  channel %d ... ", ch);
            std::fflush(stdout);
            bool ok = SetChannel(dev, ch, Rgb{255, 0, 0}, leds);
            std::wprintf(L"%ls\n", ok ? L"sent" : L"write failed");
            Sleep(static_cast<DWORD>(secs) * 1000);
            SetChannel(dev, ch, Rgb{}, leds);  // off again before the next one
        }
        std::wprintf(L"Done. Reopen Armoury Crate (or reboot) to restore your normal lighting.\n");
        return 0;
    }

    if (cmd == L"color" && args.size() >= 2) {
        Rgb c;
        if (!ParseHex(args[1].c_str(), &c)) return Usage();
        int first = 0, last = kMaxChannel;
        if (args.size() >= 3 && args[2] != L"all") first = last = _wtoi(args[2].c_str());
        std::wprintf(L"  direct mode: %ls\n", AllDirect(dev) ? L"ok" : L"write failed");
        for (int ch = first; ch <= last; ++ch)
            std::wprintf(L"  channel %d: %ls\n", ch, SetChannel(dev, ch, c, leds) ? L"sent" : L"write failed");
        std::wprintf(L"Reopen Armoury Crate (or reboot) to restore your normal lighting.\n");
        return 0;
    }
    return Usage();
}
