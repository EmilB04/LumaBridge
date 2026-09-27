// Milestone 2 check: talk to Aura directly, no game or Logitech involved.
//
//   aura-test list                 enumerate devices
//   aura-test color RRGGBB [secs]  set every light to a static color, hold, release
//   aura-test cycle [secs]         red -> green -> blue -> white, then release
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include "aura_bridge.h"
#include "color.h"
#include "log.h"

using namespace luma;

static int Usage() {
    std::fwprintf(stderr,
                  L"usage:\n"
                  L"  aura-test list\n"
                  L"  aura-test color RRGGBB [seconds]\n"
                  L"  aura-test cycle [seconds-per-color]\n");
    return 2;
}

static bool ParseHex(const wchar_t* s, Rgb* out) {
    if (*s == L'#') ++s;
    wchar_t* end = nullptr;
    unsigned long v = std::wcstoul(s, &end, 16);
    if (end == s || *end != L'\0' || std::wcslen(s) != 6) return false;
    *out = Rgb{static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    return true;
}

static void Hold(AuraBridge& aura, Rgb c, int seconds) {
    std::wprintf(L"  -> #%02X%02X%02X for %ds\n", c.r, c.g, c.b, seconds);
    if (!aura.SetAll(ToAuraColor(c))) std::fwprintf(stderr, L"  SetAll failed (see log)\n");
    Sleep(static_cast<DWORD>(seconds) * 1000);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return Usage();

    log::Init(L"", log::Level::Info);  // no file, console only
    log::SetConsoleEcho(true);

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::fwprintf(stderr, L"CoInitializeEx failed\n");
        return 1;
    }

    int rc = 0;
    {
        AuraBridge aura;
        if (!aura.Connect()) {
            std::fwprintf(stderr,
                          L"Could not connect to Aura. Is Armoury Crate (or Aura Creator /\n"
                          L"LightingService) installed and the service running?\n");
            CoUninitialize();
            return 1;
        }

        std::wprintf(L"%d Aura device(s):\n", static_cast<int>(aura.Devices().size()));
        for (size_t i = 0; i < aura.Devices().size(); ++i) {
            const auto& d = aura.Devices()[i];
            std::wprintf(L"  [%d] %-32ls type=0x%08X (%ls) lights=%d matrix=%dx%d\n", static_cast<int>(i),
                         d.name.c_str(), d.type, AuraDeviceTypeName(d.type), d.lightCount,
                         d.width, d.height);
        }

        if (_wcsicmp(argv[1], L"list") == 0) {
            // done
        } else if (_wcsicmp(argv[1], L"color") == 0 && argc >= 3) {
            Rgb c;
            if (!ParseHex(argv[2], &c)) {
                rc = Usage();
            } else {
                Hold(aura, c, argc >= 4 ? _wtoi(argv[3]) : 5);
            }
        } else if (_wcsicmp(argv[1], L"cycle") == 0) {
            int secs = argc >= 3 ? _wtoi(argv[2]) : 2;
            for (Rgb c : {Rgb{255, 0, 0}, Rgb{0, 255, 0}, Rgb{0, 0, 255}, Rgb{255, 255, 255}})
                Hold(aura, c, secs);
        } else {
            rc = Usage();
        }

        aura.Disconnect(true);
    }
    CoUninitialize();
    return rc;
}
