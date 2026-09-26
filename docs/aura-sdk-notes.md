# ASUS Aura SDK notes

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
