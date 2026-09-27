# LumaBridge

Dynamic game lighting on ASUS Aura Sync devices (motherboard, RAM, fans, GPU, Aura
keyboards like the ROG Azoth), including in games that only support other brands. No
OpenRGB, Artemis or other middleware.

![LumaBridge, Lighting page in Auto mode](docs/images/app-auto.png)

- **Auto mode:** games drive your lights. LumaBridge answers the game's lighting SDK as
  if that brand's software were installed, and follows the game that's currently active.
- **Manual mode:** color wheel, hex input, presets, breathing and strobe effects,
  brightness.
- **Calibration:** per-channel gains and gamma so Aura matches your other gear.
- **Devices:** switch individual Aura devices on or off.
- **Games:** one-click setup per SDK, with live status.
- Lives in the tray, starts with Windows if you want. The tray icon glows in the current
  color.

| Game SDK | Coverage | Your own devices of that brand |
|---|---|---|
| Logitech LIGHTSYNC | BF1 and other LIGHTSYNC titles | keep working (calls are forwarded to G HUB) |
| Razer Chroma | the largest RGB game catalogue | n/a (no Razer software needed) |
| SteelSeries GameSense | GameSense titles | n/a (built into the app) |
| Corsair iCUE (SDK 2/3) | per game folder | n/a |
| Alienware AlienFX | older titles | n/a |

Details and limits for each one are in [docs/sdk-emulators.md](docs/sdk-emulators.md).

```
 game ─LogiLed─▶ LumaBridge_x64.dll ─forward─▶ G HUB ─▶ Logitech mouse
 game ─Chroma──▶ RzChromaSDK64.dll ──┐
 game ─iCUE────▶ CUESDK.x64_2019.dll ┼─ colors (WM_COPYDATA) ─▶ LumaBridge.exe ─▶ Aura SDK ─▶ Armoury Crate
 game ─LightFX─▶ LightFX.dll ────────┘                            ▲      (motherboard, RAM, fans, Azoth)
 game ─HTTP────────────────────────────────────── GameSense ──────┘
```

When the app isn't running, each DLL drives Aura by itself, so game lighting still works
without the UI.

## Status

Everything compiles for x64 and x86 (mingw-w64 cross-build, and MSVC in CI). The
platform-independent logic (color math, effects, Chroma/Corsair/LightFX/GameSense
translation, JSON, HTTP, IPC, source selection) is covered by unit tests. **Nothing has
been run against real hardware yet.** Start with the checks under
[Verify on hardware](docs/roadmap.md#verify-on-hardware-next).

| Milestone | State |
|---|---|
| 1. LogiLed proxy with pass-through | implemented |
| 2. Aura bridge (`aura-test.exe`) | implemented |
| 3. Ambient mirror (static, per-key frames reduced, flash/pulse) | implemented |
| 3b. Chroma, GameSense, Corsair, LightFX front-ends | implemented |
| 5. App: Auto/Manual, calibration, devices, integrations, tray, autostart | implemented |
| 4. Per-key output on the Azoth | planned ([roadmap](docs/roadmap.md)) |

## Quick start

1. Install Armoury Crate (Aura's lighting service is required; there's no way around it).
2. Build ([docs/building.md](docs/building.md)) or download the `LumaBridge-x64` artifact
   from the latest GitHub Actions run, and unzip it anywhere, e.g.
   `C:\Program Files\LumaBridge`.
3. Run `LumaBridge.exe`. Under **Games**, set up the SDKs you want (Logitech is one
   click; Chroma and AlienFX ask for admin once; Corsair is per game).
4. Start a game. It appears on the **Lighting** page, and your Aura devices follow it.

Troubleshooting: **Settings → Open log folder** (`%LOCALAPPDATA%\LumaBridge`). Each piece
writes its own log (`lumabridge-app.log`, `lumabridge.log` for the Logitech proxy,
`lumabridge-chroma.log`, …).

## Repo layout

```
src/app            LumaBridge.exe: UI (Dear ImGui + D3D11), tray, controller, integrations
src/aura-bridge    Aura COM wrapper (late-bound IDispatch) + mirror worker thread
src/proxy-dll      Logitech LED SDK proxy
src/chroma-emu     Razer Chroma emulator
src/corsair-emu    Corsair CUE SDK emulator
src/lightfx-emu    Alienware LightFX emulator
src/gamesense      SteelSeries GameSense engine + HTTP server
src/common         color math, effects, config, log, JSON, IPC (mostly portable, unit tested)
tools/             aura-test (list/set Aura devices), logiled-harness (fake LIGHTSYNC game)
tests/             unit tests (run on any OS)
scripts/           install helpers the app calls (also usable by hand)
docs/              SDK notes, build guide, roadmap, screenshots
```

## Known limits

- **Armoury Crate is required.** Aura hardware access goes through ASUS's service.
- **Some newer Chroma titles verify Razer's signature** and ignore the emulator.
- **Corsair iCUE SDK 4** (2022+ titles) and **Windows 11 Dynamic Lighting** aren't
  covered. See [docs/sdk-emulators.md](docs/sdk-emulators.md) for why.
- **Anti-cheat:** nothing touches game memory. The DLLs are ordinary libraries the game
  loads on purpose, the same technique Aurora and Artemis have used for years. Still,
  check a kernel anti-cheat title before trusting it with your account.
