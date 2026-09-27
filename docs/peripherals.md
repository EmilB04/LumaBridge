# Keyboards and mice

## Logitech (G502 X Plus and other Logitech RGB gear)

LumaBridge loads G HUB's own LED SDK DLL (`LGHUB\sdks\sdk_legacy_led_x64.dll`, found
through the machine-wide `ServerBinary` registration and never LumaBridge's own proxy) in
the app's process. It calls `LogiLedInitWithName("LumaBridge")`,
`LogiLedSetTargetDevice(all)` and `LogiLedSetLighting(r%, g%, b%)`, and G HUB does the
device I/O. `LogiLedShutdown` hands the devices back to G HUB's profile. That happens
whenever LumaBridge isn't controlling the lights, or a game lights Logitech gear itself.
Code: [`src/app/peripherals/logitech_output.cpp`](../src/app/peripherals/logitech_output.cpp).

## ASUS ROG Azoth (wired or wireless, experimental)

Captured from Armoury Crate with USBPcap on a wired Azoth (USB `0B05:1A83`, firmware
rev 0418). The vendor interface is `MI_01`, usage page `0xFF00`, 64-byte reports with
report ID 0, and the commands go on interrupt OUT endpoint 2.

| Command (data bytes) | Meaning |
|---|---|
| `51 2C 00 00 FF <bright> 00 FF FF <R> <G> <B>` | Static color. Brightness `0x00`–`0x64`. Seen for white `FF FF FF`, red `FF 00 2C` and blue `00 03 FF`, at brightness `0x32` and `0x64`. |
| `50 55` | Save to the keyboard's flash (Armoury Crate sends it after every change). **LumaBridge never sends it.** |
| `12 xx`, `22 xx`, `7D 20 02` | Armoury Crate's periodic status queries. |

LumaBridge sends only the static-color command, at most about 10 times a second, with its own
brightness applied to the color. Because it never saves, the lighting Armoury Crate stored
in the keyboard is untouched and comes back when the keyboard restarts.

### Wireless (ROG Omni receiver `0B05:1ACE`)

Captured over 2.4 GHz: Armoury Crate sends the **same commands** to the receiver's vendor
collection (`MI_02`, `Col02`, usage page `0xFF00`) as 64-byte output reports with
**report ID 2** (63 data bytes), on interrupt OUT endpoint 3; the receiver echoes each one
on IN endpoint 3 and passes it on to the keyboard. For example green at 100 %:
`02 51 2C 00 00 FF 64 00 FF FF 00 FF 00`, and the save is `02 50 55` (never sent).
Report ID 1 (`Col01`, usage page `0xFF02`) talks to the receiver itself (`01 A0` version,
`01 A1` serial); LumaBridge doesn't use it.

LumaBridge tries the cable first and falls back to the receiver. With the keyboard asleep
or switched off, the receiver accepts the command and nothing changes; the keyboard picks
up the color within the next 5-second refresh once it wakes.

Not yet:
- **Per-key colors:** needs a capture of per-key lighting from Armoury Crate.
- **Handing back without a restart:** needs the replies to the `12 xx` queries, which
  would let LumaBridge read and restore the saved color.

`tools\device-probe.exe` lists the HID interfaces of ASUS and Logitech devices (read-only).
