# Keyboards, mice and RAM

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

## RAM: HyperX / Kingston FURY RGB DDR4 (experimental)

The sticks' lighting controller answers at SMBus address `0x27` (one controller for all
sticks, 5 LEDs each). The protocol is the one OpenRGB documents
(`Controllers/HyperXDRAMController`); LumaBridge's own code for it is
[`src/app/peripherals/hyperx_ram.h`](../src/app/peripherals/hyperx_ram.h):

| Register | Meaning |
|---|---|
| `E1 = 01` | Start an update |
| `E5 = 21` | Direct mode (each LED its own color) |
| `base + 3*led + 0/1/2` | Red / green / blue of an LED. `base` = `0x11`, `0x41`, `0x71`, `0xA1` for SPD slots 0-3 |
| `base + 0x10 + 3*led` | Brightness of an LED, 0-100 (LumaBridge sends 100 and dims the color itself) |
| `E1 = 02`, `E1 = 03` | Apply |

**How it runs.** Only administrators can reach the SMBus. LumaBridge reaches it through
[PawnIO](https://pawnio.eu), a small signed driver made for this, which runs only small
signed modules (here `SmbusPIIX4`, AMD chipsets). PawnIO's installer and modules come with
LumaBridge, unmodified ([third_party/pawnio](../third_party/pawnio/README.md)). The app
(running as you) doesn't touch the hardware. **`LumaBridge-Helper.exe`** does, as a scheduled
task that runs as SYSTEM. It talks to the PawnIO driver directly through its device interface,
so it needs no PawnIO library.

`scripts/Install-Helper.ps1` sets everything up in one step: Devices → **Hardware access**
→ Set up, with one administrator prompt. Switching on **Light the RAM** also runs it.
1. It installs PawnIO silently (`PawnIO_setup.exe -install -silent`) unless PawnIO 2 or
   later is already installed.
2. It copies the helper and its modules into `%ProgramFiles%\LumaBridge`, so nobody without
   administrator rights can swap the file the task runs.
3. It registers the task and lets your account start it.

The app starts the helper whenever it runs. It renders its effect across each stick's 5 LEDs
and passes the colors through shared memory (`Global\LumaBridgeHelper`). The helper exits
when the app exits.

**Safety.**
- Finding the sticks only reads: "receive byte" from `0x27` and from the SPD chips
  `0x50`-`0x53`, and the memory-type byte (DDR4 = `0x0C`) of each SPD. The firmware's
  memory list must name Kingston / HyperX. None of this happens until RAM lighting is
  switched on.
- Writes go only to `0x27`, and only to the registers above: every write passes
  `ram::IsAllowed()` (tested), whatever the app asks. The SPD chips are never written.
- Each frame holds the system-wide SMBus lock (`Access_SMBUS.HTP.Method`) that Armoury
  Crate, HWiNFO and others use.
- After 5 failed frames in a row the helper stops. Log: `%ProgramData%\LumaBridge\helper.log`.

**Not yet:** Intel chipsets (PawnIO's `SmbusI801` module), Kingston FURY DDR5 (a different
controller) and handing the RAM back to Armoury Crate (the sticks keep the last color until
Armoury Crate sets them again or the PC restarts). If the sticks flicker, Armoury Crate is
lighting them too: switch the RAM off in Armoury Crate.

## Sensors without LibreHardwareMonitor

The same helper reads the dashboard's fan speeds and CPU / board temperatures
([`src/app/peripherals/hw_sensors.h`](../src/app/peripherals/hw_sensors.h), tested):

- **CPU temperature (AMD Ryzen):** `THM_TCON_CUR_TMP` (SMN `0x59800`) through PawnIO's
  `AMDFamily17` module. Bits 31:21 are the temperature in 1/8 °C, and 49 °C is subtracted
  when the range-select bit is set. That's Tctl, the value Ryzen Master and LibreHardwareMonitor
  show.
- **Board (Nuvoton NCT6796D / 6797D / 6798D / 6799D, on most ASUS AMD boards):** found on
  the Super I/O ports `0x2E` / `0x4E` through PawnIO's `LpcIO` module. It enters
  configuration mode, reads the chip id, and reads the hardware monitor's base address
  (logical device `0x0B`). The chip's I/O space lock is cleared, as the Linux driver does.
  Then the helper only reads the banked registers:
  - fan counts at `0x4B0`-`0x4BA`, `0x4CC`, converted as RPM = 1 350 000 / count;
  - temperatures: `0x490` Motherboard (SYSTIN) and `0x491` CPU Socket (CPUTIN).

  Fan names follow the usual ASUS wiring of these chips (Chassis 1, CPU, Chassis 2 and 3,
  CPU optional, AIO pump); check them against your BIOS.
- Each read holds the shared ISA-bus or PCI lock. Other monitoring chips aren't supported yet;
  LibreHardwareMonitor's web server still works as a fallback.
