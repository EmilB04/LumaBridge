# LumaBridge

Bridges game lighting SDK calls to ASUS Aura Sync devices (motherboard, RAM, fans, GPU,
Aura keyboards like the ROG Azoth) without OpenRGB, Artemis or any other middleware.

Two front-ends feed one Aura back-end:

| Front-end | Replaces | Games | Your Logitech gear |
|---|---|---|---|
| **LogiLed proxy** (`LumaBridge_x64.dll`) | Logitech LED SDK DLL (LIGHTSYNC) | BF1 and other LogiLed titles | keeps working (every call is forwarded to G HUB's DLL first) |
| **Chroma emulator** (`RzChromaSDK64.dll`) | Razer Chroma SDK runtime | the large Chroma catalogue | n/a — no Razer hardware or Synapse needed |

```
 game ──LogiLed calls──▶ LumaBridge_x64.dll ──forward──▶ G HUB sdk_legacy_led_x64.dll ─▶ Logitech mouse
                               │
 game ──Chroma calls──▶ RzChromaSDK64.dll (emulated)
                               │  (ambient color, coalesced, ≤30 Hz, worker thread)
                               ▼
                     Aura COM SDK (aura.sdk.1) ─▶ LightingService ─▶ motherboard / RAM / fans / Azoth
```

## Status

| Milestone | State |
|---|---|
| 1. Proxy DLL, all 32 LogiLed exports, pass-through to the real DLL | implemented, needs on-hardware verification |
| 2. Aura init / enumerate / static color (`aura-test.exe`) | implemented, needs on-hardware verification |
| 3. Single-color ambient mirror (static, bitmap, zones, flash/pulse, save/restore) | implemented, needs on-hardware verification |
| 3b. Razer Chroma emulator → same ambient mirror | implemented, needs on-hardware verification |
| 4. Azoth per-key mapping | planned ([docs/roadmap.md](docs/roadmap.md)) |
| 5. Tray UI / autostart | planned; everything is configured in `LumaBridge.ini` for now |

Everything compiles for x64 and x86 (checked with mingw-w64 and in CI with MSVC), and the
platform-independent core has unit tests. Nothing has been run against real hardware yet.

## Quick start

1. **Build** in Visual Studio 2022 (open the folder, pick preset `vs-x64`) or download the
   `LumaBridge-x64` artifact from the GitHub Actions run. See [docs/building.md](docs/building.md).
2. **Check Aura works on its own:** `aura-test.exe list`, then `aura-test.exe cycle`.
   Armoury Crate (or its LightingService) must be installed.
3. **Check pass-through without a game:**
   `logiled-harness.exe "C:\Program Files\LGHUB\sdk_legacy_led_x64.dll"` (real DLL: the mouse
   should cycle colors), then `logiled-harness.exe LumaBridge_x64.dll` (mouse *and* Aura).
4. **Hook the game:**
   - LogiLed / BF1: `.\Install-LogiLedProxy.ps1` (per-user registry redirect, no admin).
     See [docs/logiled-exports.md](docs/logiled-exports.md#how-games-find-the-dll) for how
     to confirm which DLL BF1 really loads, and the fallback modes.
   - Chroma games: `.\Install-ChromaEmulator.ps1` (admin, installs into System32) or
     `-GameDir <folder>` for a single game. See [docs/chroma-emulation.md](docs/chroma-emulation.md).
5. Copy `LumaBridge.ini.example` → `LumaBridge.ini` to tune brightness, color gains or
   device selection, and read `%LOCALAPPDATA%\LumaBridge\lumabridge*.log` if nothing lights up.

## Repo layout

```
src/common        color math, lighting state machine (portable, unit tested), config, log
src/aura-bridge   Aura COM wrapper (late-bound IDispatch) + mirror worker thread
src/proxy-dll     LogiLed proxy: exports, real-DLL loader
src/chroma-emu    Razer Chroma SDK emulator
tools/            aura-test (milestone 2), logiled-harness (fake game)
tests/            core unit tests (run on any OS)
scripts/          install / uninstall helpers (PowerShell)
docs/             SDK notes, build guide, roadmap
third_party/      optional reference headers (not needed to build, not committed)
```

## Known limits and risks

- **Armoury Crate stays a dependency.** Aura hardware access goes through ASUS's
  LightingService. There is no supported way around it.
- **Game updates** can change how a game finds its lighting DLL. Check the log after updates.
- **Some newer Chroma titles verify the Razer signature** on `RzChromaSDK64.dll` and will
  refuse the emulator. See [docs/chroma-emulation.md](docs/chroma-emulation.md#signature-checks).
- **Anti-cheat.** Neither DLL touches game memory. They are ordinary libraries the game
  loads on purpose, the same technique Aurora and Artemis have used for years. Still, check
  a kernel anti-cheat title before trusting it with your account.
