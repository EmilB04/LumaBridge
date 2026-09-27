# SDK emulators

The goal is dynamic game lighting on hardware the game doesn't support. LumaBridge answers
each game's lighting SDK as if the vendor's software and devices were installed, then
sends the colors to Aura.

| SDK | How games reach it | LumaBridge piece | Install (Games page does this) |
|---|---|---|---|
| Logitech LIGHTSYNC | LED SDK DLL found via registry | `LumaBridge_x64.dll` proxy (see [logiled-exports.md](logiled-exports.md)) | per-user registry redirect |
| Razer Chroma | `RzChromaSDK64.dll` from System32 | Chroma emulator DLL | System32, admin once |
| SteelSeries GameSense | HTTP to the address in `coreProps.json` | built into `LumaBridge.exe` | nothing (admin once only if the folder isn't writable) |
| Corsair iCUE (SDK 2/3) | `CUESDK*.dll` shipped in the game folder | Corsair emulator DLL | per game folder |
| Alienware AlienFX | `LightFX.dll` from System32 | LightFX emulator DLL | System32, admin once |

All of them feed the same place. While the LumaBridge app runs, the DLLs inside games
send their colors to the app (`WM_COPYDATA`, [`src/core/ipc.h`](../src/core/ipc.h)).
The app is then the only thing talking to Aura, and it decides what to show (Auto mode:
most recently active game; Manual mode: your color). Without the app, the DLLs leave Aura
alone. ASUS's Aura library runs inside whichever process calls it, and a crash in one of
its device plug-ins (seen with the Azoth's `AacKbHal_x64.dll`) would kill the game.
`[Aura] DirectFromGames=1` restores the old stand-alone behaviour.

## Can a game think an Azoth is a Razer (or Corsair) keyboard?

Yes. **Chroma** games don't drive "a BlackWidow TKL". They send effects to device
*classes* (a generic 6×22 keyboard grid, mouse, headset, mousepad, Chroma Link), and the
emulator answers "connected" to every model a game asks about. **Corsair** games enumerate
devices, so the emulator reports a virtual full-size keyboard (104 LEDs with real
positions) and a 4-zone mouse, and the game paints those. **GameSense** games only
describe *what* to show ("health on the function keys, red→green"), and LumaBridge renders
that itself. In every case the result becomes one ambient color on all your Aura devices,
Azoth included. Per-key output on the Azoth is milestone 4.

## Razer Chroma

`RzChromaSDK64.dll` / `RzChromaSDK.dll` ([`src/integrations/razer`](../src/integrations/razer)).

| Export | Behavior |
|---|---|
| `Init`, `InitSDK(appInfo)` | start, return success |
| `UnInit` | stop, release Aura |
| `Create{Keyboard,Mouse,Headset,Mousepad,Keypad,ChromaLink}Effect` | translate to a color; apply now (`pEffectId == NULL`) or store it |
| `CreateEffect(deviceGuid, ...)` | generic `CHROMA_STATIC` / `CHROMA_NONE` |
| `SetEffect` / `DeleteEffect` | apply / forget a stored effect |
| `QueryDevice` | "connected" (`[Chroma] ReportDevicesConnected`) |
| `Register/UnregisterEventNotification` | no-op success |

The emulated effect types are `STATIC`, `CUSTOM`, `CUSTOM_KEY`, `CUSTOM2` and `NONE`.
Modern Chroma games, including Razer's Unity/UE plugins and `.chroma` animations, render
every frame as `CUSTOM` grids, so the dynamic effects are covered. The deprecated hardware
effects (wave, spectrum, breathing) are accepted but not animated. Ambient source:
`[Chroma] AmbientSource` (auto = keyboard, then Chroma Link, …).

**Signature checks:** some newer Chroma titles verify that `RzChromaSDK64.dll` is signed
by Razer, and will skip Chroma with the emulator installed. No unsigned DLL can get past
that. The fallback would be reading the genuine Razer runtime's effect stream (Artemis
does this), which isn't built yet.

## SteelSeries GameSense

Built into the app ([`src/integrations/steelseries`](../src/integrations/steelseries)). On start it listens on
`127.0.0.1:49713` (loopback only, `[GameSense] Port`) and writes
`%ProgramData%\SteelSeries\SteelSeries Engine 3\coreProps.json`. On exit it removes the
file, or restores SteelSeries GG's copy if there was one.

Implemented endpoints: `game_metadata`, `register_game_event`, `bind_game_event`,
`game_event`, `multiple_game_events`, `game_heartbeat`, `remove_game_event`,
`remove_game`, `stop_game`. Other endpoints (screen, tactile, GoLisp) are accepted and
ignored.

Handlers are evaluated the way GameSense does it:

- **Colors:** static, `gradient` (zero→hundred over the event's min..max), or value
  `ranges`.
- **Modes:** `color`, `percent`/`count` (dark at 0), `context-color` (from
  `data.frame`), and `bitmap`/`partial-bitmap` (full-keyboard frames, reduced like other
  per-key frames).
- **Flashing:** `rate.frequency`, a fixed number or value ranges.

Several events usually light different zones at once. The ambient color comes from the
most "whole-rig" handler: full-keyboard bitmap > `rgb-1-zone` > whole keyboard > other
RGB zones > single keyboard zones > mouse and headset. Ties go to the most recent. A game
goes inactive after its `deinitialize_timer_length_ms` (default 15 s) without events.

**With SteelSeries GG installed**, LumaBridge sits in front of GG instead of replacing
it. It saves GG's `coreProps.json`, answers games itself (so Aura reacts), and passes
every request on to GG in order on a background thread. GG keeps working, including
SteelSeries devices and **Moments** auto-clips, and a slow or closed GG never delays
the game. If GG restarts and rewrites `coreProps.json` with a new port, LumaBridge
notices within 2 seconds, follows the new port, and puts its own file back. On exit,
GG's file is restored. Forwarding only ever goes to a loopback address, and
`[GameSense] ForwardToGG=0` turns it off.

## Corsair iCUE (CUE SDK 2.x / 3.x)

`CUESDK.x64_2019.dll` / `CUESDK_2019.dll` ([`src/integrations/corsair`](../src/integrations/corsair)).
Games ship this DLL themselves, so it's installed per game. **Games → Corsair → Add to a
game…** finds every `CUESDK*.dll` in the folder you pick, backs it up and replaces it.

Implemented: handshake, device count/info, LED positions (per device), key-name lookup,
`SetLedsColors` (+ `Async`, buffered `ByDeviceIndex` + `Flush`/`FlushAsync`),
`GetLedsColors`, request/release control, layer priority, last error, and no-op event and
keypress subscriptions. Property lookups return "not available".

The virtual devices' LED ids are LumaBridge's own, returned through the SDK's lookups.
Games that hard-code Corsair's `CLK_*` numbers still work for the ambient color, but would
light the wrong key once per-key output exists; the real id table is a milestone 4 item.

**iCUE SDK 4.x** (`iCUESDK.x64_2019.dll`, 2022+ titles) is a different, session-based
API and isn't emulated yet. The installer warns when a game uses it.

## Alienware AlienFX (LightFX)

`LightFX.dll` ([`src/integrations/alienware`](../src/integrations/alienware)): one virtual desktop device with
one light that covers every location. It supports the buffered
`Light`/`SetLightColor` + `Update` model, the brightness byte, `ActionColor(Ex)` pulse and
morph (morph jumps to the end color), and `SetTiming`. There are few titles, mostly
older ones.

## Not possible this way: Windows 11 "Dynamic Lighting"

Windows 11 Dynamic Lighting (Settings → Personalization → Dynamic Lighting) isn't a
vendor SDK. Games talk to *HID LampArray* devices through Windows itself. Faking one
would need a virtual HID device driver (kernel/UMDF, signed), which is out of scope for
LumaBridge. If your Azoth shows up on that Settings page, those games already reach it
directly without LumaBridge.

## Several SDKs in one game

Some games support two or more SDKs. With the app running this is harmless: each shows
up as its own source and Auto mode follows the one that changed most recently. Without
the app, the DLLs would overwrite each other on Aura. Remove all but one for that game.
