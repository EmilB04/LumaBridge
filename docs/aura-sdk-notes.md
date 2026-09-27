# ASUS Aura notes

## Direct USB (default)

LumaBridge talks to the motherboard's **AURA LED Controller** over USB HID, without
Armoury Crate or the SDK below ([`src/aura-usb`](../src/aura-usb)). This was confirmed on a
ROG STRIX B550-F (USB `0B05:1939`, firmware `AULA3-AR42-0222`):

- 65-byte reports with id `0xEC`. `EC 82` returns the firmware and `EC B0` the configuration
  table (1 ARGB header, 5 board LEDs, 2 RGB headers on that board).
- **Effect channels and direct channels are numbered differently.** Switch every effect
  channel to direct mode (`EC 35 <ch> 00 00 FF`), then paint direct channels
  (`EC 40 ...`). Direct channel **0** is the ARGB header (fans through a passive hub) and
  **4** is the board's own LEDs.
- LumaBridge enters direct mode once per connection and then re-sends the current frame
  every second, so a color overwritten by ASUS's lighting service comes back quickly.
  Re-sending the *mode* command periodically blanked the LEDs for an instant (a visible
  flicker every 2 s on a B550-F), so it isn't repeated.
- Nothing is ever written to the controller's flash.
- **Handing back:** the controller has no "give control back" command, and restarting
  ASUS's lighting service doesn't restore Armoury Crate's effect. Starting the Armoury
  Crate app does (it re-applies its current Aura Sync effect). So handing back means:
  stop sending colors, then start Armoury Crate. LumaBridge does this for the idle choice
  "Armoury Crate", for **Stop controlling the lights**, and when you exit it. (While Armoury
  Crate's services were hung after SDK crashes, only a reboot helped.)
- Armoury Crate lists every program that has used the Aura SDK under **Game list**
  (lighting priority) above its own "Aura Sync". Programs that crashed while holding SDK
  control left Aura Sync in standby and its services hung until a reboot. LumaBridge's
  app no longer uses the SDK.
- RAM (e.g. HyperX Fury) sits on SMBus, not USB, and would need a kernel driver, so it
  isn't supported.

`aura-usb-test.exe` and `Run-HardwareTest.ps1` probe all of this on a new board.

# ASUS Aura SDK notes (legacy, `[Aura] Backend=sdk`)

> On current Armoury Crate versions the SDK reports **0 devices**, and listing devices can
> fail-fast the calling process inside ASUS's `AacKbHal_x64.dll` (Azoth keyboard plug-in).
> It is kept only for older setups.

Code: [`src/aura-bridge/aura_bridge.cpp`](../src/aura-bridge/aura_bridge.cpp).

## Access model

- COM server, ProgID **`aura.sdk.1`**, type library *AuraServiceLib*, hosted by ASUS
  **LightingService** (installed with Armoury Crate, or with Aura Creator / the legacy Aura
  app). If the service isn't installed or running, `CLSIDFromProgID`/`CoCreateInstance` fail
  and the mirror retries every 5 s.
- LumaBridge uses **late-bound `IDispatch`** (`GetIDsOfNames` + `Invoke`), the same way the
  well-known Python `win32com.client.Dispatch("aura.sdk.1")` samples do. So builds need no
  ASUS headers or `#import`, and small interface revisions don't break the binary.

## Object model used

```
IAuraSdk                      (aura.sdk.1)
  SwitchMode()                take control from Armoury Crate (required before writing)
  Enumerate(0) -> collection  0 = all device types
  ReleaseControl(0)           hand control back
IAuraSyncDeviceCollection     Count, Item(i)
IAuraSyncDevice               Name, Type, Width, Height, Lights, Apply()
IAuraRgbLightCollection       Count, Item(i)
IAuraRgbLight                 Color (DWORD 0x00BBGGRR), Red, Green, Blue, Name
```

Keyboards (`IAuraSyncKeyboard`) also expose `Keys` / `Key(scanCode)` with per-key
`Color`. Milestone 4 will use that for the Azoth.

## Device type codes

| Code | Name (`DeviceTypes=` key) |
|---|---|
| 0x00010000 | motherboard |
| 0x00011000 | motherboard_ledstrip (ARGB headers → fans/strips) |
| 0x00020000 | aio |
| 0x00030000 | vga / gpu |
| 0x00040000 | display |
| 0x00050000 | headset |
| 0x00060000 | microphone |
| 0x00070000 | hdd |
| 0x00080000 | bd |
| 0x00090000 | dram / ram |
| 0x000A0000 | keyboard |
| 0x000A0001 | nb_keyboard |
| 0x000A0011 | nb_keyboard_4zone |
| 0x000B0000 | mouse |
| 0x000C0000 | chassis |
| 0x000D0000 | projector |

`aura-test.exe list` prints what your machine actually reports. Paste that output here
once you've run it, because it settles whether the Azoth shows up as an Aura keyboard and
with what matrix size.

## Gotchas

- Only one process should call `SwitchMode()` at a time. That's why the LumaBridge app
  is the single Aura owner while it runs, and game DLLs send their colors to it instead
  of opening their own Aura session.
- `SwitchMode()` takes over **all** Aura devices, including ones the config excludes,
  which then freeze on their last color. `ReleaseControl` (on `LogiLedShutdown` / `UnInit`)
  gives them back.
- If the game is killed without shutting the SDK down, LightingService should notice the
  dead COM client and resume Armoury Crate effects, though maybe not immediately. Opening
  Armoury Crate forces it.
- `Apply()` is an out-of-process call, typically a few ms per device. That's why writes are
  coalesced on a worker thread and capped (`MaxUpdateHz`, default 30).
- Aura and Logitech LEDs render the same RGB differently. Use `[Color]` gains and gamma
  to match them by eye.
