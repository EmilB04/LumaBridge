# Razer Chroma emulation (and why not Corsair, yet)

Goal: get dynamic game lighting onto hardware the game doesn't support by answering the
game's lighting SDK as if the vendor's software and devices were present.

## Can a game be made to think an Azoth is a Razer TKL?

Yes, and you don't even have to impersonate a particular model. Chroma games don't
drive "a BlackWidow TKL". They send effects to device **classes** through the SDK runtime
`RzChromaSDK64.dll`:

- `CreateKeyboardEffect(CHROMA_CUSTOM, Color[6][22])`: one generic 6×22 keyboard grid,
  used for every Razer keyboard layout
- `CreateMouseEffect`, `CreateHeadsetEffect`, `CreateMousepadEffect`, `CreateKeypadEffect`
- `CreateChromaLinkEffect(Color[5])`: 5 "ambient" LEDs intended for third-party gear

The game never learns what's plugged in unless it calls `QueryDevice(<model GUID>)`, and
the emulator answers "connected" to every model (`[Chroma] ReportDevicesConnected=1`).
So any game that calls `Init()` successfully will send its effects, and LumaBridge turns
them into Aura colors.

## What's implemented

`RzChromaSDK64.dll` / `RzChromaSDK.dll` built from [`src/chroma-emu`](../src/chroma-emu):

| Export | Behavior |
|---|---|
| `Init`, `InitSDK(appInfo)` | start the Aura mirror, return success |
| `UnInit` | stop mirror, release Aura to Armoury Crate |
| `Create{Keyboard,Mouse,Headset,Mousepad,Keypad,ChromaLink}Effect` | translate to a color; apply now (`pEffectId == NULL`) or store it |
| `CreateEffect(deviceGuid, ...)` | generic `CHROMA_STATIC` / `CHROMA_NONE` only |
| `SetEffect(id)` / `DeleteEffect(id)` | apply / forget a stored effect |
| `QueryDevice` | "connected" (configurable) |
| `Register/UnregisterEventNotification` | no-op success |

Emulated effect types are `STATIC`, `CUSTOM`, `CUSTOM_KEY`, `CUSTOM2` and `NONE` (off).
Modern Chroma games (including everything built on Razer's Unity/UE plugins and
`.chroma` animations) render every frame as `CUSTOM` grids, so this covers the dynamic
effects. The deprecated hardware effects (wave, spectrum, breathing, reactive) are
accepted but not animated.

**Ambient selection** (`[Chroma] AmbientSource=auto`): the keyboard grid wins if the game
sends one, then Chroma Link, headset, mousepad, keypad, mouse. Grids reduce to one color
with the same `[Mirror] BitmapMode` (average of lit keys, or brightest key) as the LogiLed
path.

The enum values and struct layouts are re-declared in
[`chroma_types.h`](../src/chroma-emu/chroma_types.h) from the public SDK docs. Keyboard and
Chroma Link are the ones that matter and the ones I'm most confident about. If a game
shows wrong colors on the mouse, headset or mousepad paths, compare that file against a
current `RzChromaSDKTypes.h` first. Reads are bounded so that a wrong enum value can only
produce a wrong color, never an out-of-bounds read.

## Install

```powershell
# every Chroma game (admin; refuses to overwrite a real Razer-signed DLL without -Force)
.\Install-ChromaEmulator.ps1
# or one game only
.\Install-ChromaEmulator.ps1 -GameDir "D:\Games\SomeGame"
.\Install-ChromaEmulator.ps1 -Uninstall
```

Games `LoadLibrary("RzChromaSDK64.dll")` by bare name, so Windows checks the game folder
first and then System32. You have no Razer software, so System32 has no real DLL to
conflict with, which makes the global install clean.

Config lives in `%LOCALAPPDATA%\LumaBridge\LumaBridge.ini`; the log is
`lumabridge-chroma.log` next to it.

## Signature checks

Razer's newer sample code and engine plugins can verify that `RzChromaSDK64.dll` is
Authenticode-signed by Razer before loading it. A game that does this silently skips
Chroma with the emulator installed, and no unsigned DLL can get around it. When a game
shows nothing in `lumabridge-chroma.log`, this is the most likely reason.

The robust fallback for those games is the route Artemis takes: install Razer's **Chroma
SDK Core / Synapse** (it runs without Razer hardware), let the game talk to the genuine,
signed runtime, and read the effect stream it publishes. That would be a third front-end
(`src/chroma-reader`), not started yet. See the roadmap.

## Both SDKs in one game

Some games support both LogiLed and Chroma. If both LumaBridge DLLs load, each runs its
own Aura mirror and they overwrite each other. Disable one per game (install only one, or
`[Aura] Enabled=0` in a game-folder `LumaBridge.ini`).

## Corsair iCUE: assessment

Possible, but less valuable and more work, so it isn't implemented yet:

- **Where the DLL lives:** games ship `CUESDK.x64_2019.dll` (or `CUESDK_2015.dll`, …) in
  their own folder, so a game-folder replacement is easy and there are no signature
  checks. This part is easier than Chroma.
- **Harder part:** iCUE games enumerate devices (`CorsairGetDeviceCount`,
  `CorsairGetDeviceInfo`, `CorsairGetLedPositions`) and address individual LED IDs, so
  the emulator has to invent a believable device (e.g. a K70 with its full LED table) and
  the game only lights what it believes exists. Three incompatible ABI generations
  (SDK 2.x, 3.x, 4.x) would each need their own emulator.
- **Coverage:** Chroma's game list is several times iCUE's, and many iCUE titles also
  support Chroma.

Recommendation: prove LogiLed + Chroma on real hardware first, then add a Corsair SDK 3.x
emulator (the most common ABI in games) if a specific game you play needs it.
