# Keyboards, mice and RAM

## Logitech (G502 X Plus and other Logitech RGB gear)

LumaBridge loads G HUB's own LED SDK DLL (`LGHUB\sdks\sdk_legacy_led_x64.dll`, found
through the machine-wide `ServerBinary` registration and never LumaBridge's own proxy) in
the app's process. It calls `LogiLedInitWithName("LumaBridge")`,
`LogiLedSetTargetDevice(all)` and `LogiLedSetLighting(r%, g%, b%)`, and G HUB does the
device I/O. `LogiLedShutdown` hands the devices back to G HUB's profile. That happens
whenever LumaBridge isn't controlling the lights, or a game lights Logitech gear itself.
Code: [`src/app/peripherals/logitech_output.cpp`](../src/app/peripherals/logitech_output.cpp).

### Mouse zones (test)

The mouse gets one color today. To find out whether G HUB lets an app color the G502 X
Plus's zones separately, exit LumaBridge and run `tools\logiled-harness.exe --zones`
(add a number for the seconds per step; the default is 2). It loads G HUB's DLL and calls
`LogiLedSetLightingForTargetZone` for mouse zones 0-7, after a whole-mouse control step.
Result on a G502 X Plus (G HUB SDK 75.71.76): G HUB accepts zones 0 and 1 and refuses 2-7,
but the strip only ever shows one color. Whole-mouse colors (`LogiLedSetLighting`) work; zone
colors don't split the strip. So through the LED SDK this mouse is one color, which is what
LumaBridge sends. G HUB's own effects (cycle, breathing, color wave, ripple, screen sampler,
audio visualizer) do light the strip LED by LED, but the SDK doesn't offer them to apps;
`tools\ghub-probe.exe` (read-only) can show whether G HUB's local WebSocket connection
switches them.

What `ghub-probe` found (G HUB with a G502 X Plus over LIGHTSPEED):
- `lghub_agent.exe` serves JSON over a WebSocket on `127.0.0.1:9010`. `GET /devices/list`
  and `GET /applications` answer, and the battery broadcasts come in. `/lighting/state`,
  `/lighting/current`, `/devices/state`, `/sdk/state`, `/profiles/active` and `/games/state`
  don't exist.
- The mouse is `deviceCategory: MOUSE_RGB_PER_KEY`, `isPerKey: true`, with two zone types:
  `ZONE_PRIMARY` (off, fixed, cycle, breathing) and `ZONE_ALL` (color wave, custom,
  Logitech's signature effects).
- Switching effects in G HUB changed only the battery report's lighting power use. None of
  the lighting paths the probe subscribed to reported anything, so the paths G HUB's window
  uses to set effects are still unknown.
- G HUB also runs `logi_lamparray_service`, which offers its devices to Windows Dynamic
  Lighting. The G502 X Plus shows up there, but Windows only offers it one color at a time,
  with no effects, so that route is one color too.

### G502 X Plus over HID++ (what G HUB sends)

A USB capture of G HUB changing the mouse's effects (through its LIGHTSPEED receiver
`046D:C547`) shows standard Logitech HID++ 2.0 long reports to device index 1:
`11 01 <feature index> <function << 4 | software ID> <parameters>`. On this mouse and
firmware the RGB effects feature (`0x8071`) is at index `09`. G HUB set every effect with its
function 1, SetRgbClusterEffect: `<cluster> <effect index> <10 parameters> <01>`.

| Cluster | Effect | Parameters | G HUB |
|---|---|---|---|
| `00` | `01` | `R G B 02 00…` | Fixed |
| `00` | `02` | `R G B <period ms, big-endian> 00 <intensity 0-100> 00 00 00` | Breathing |
| `00` | `03` | `00 00 00 00 00 <period ms, big-endian> <intensity> 00 00` | Cycle |
| `FF` | `00` | `00 ×6 <period low byte> 01 <intensity> <period high byte>` | Color wave |
| `FF` | `02` | `00 ×6 <20–24> 64 00 00` | Pulsarpunkt (Logitech signature effect) |

Examples of color wave periods: 5000 ms (`88 … 13`), 9400 ms (`B8 … 24`) and 5500 ms
(`7C … 15`), plus intensity `53` (83 %). After each effect G HUB also sent `10 01 08 2B 00 01 00`, but
index 8 is feature `0x2121`, the scroll wheel, so that's unrelated to lighting. The mouse
has one cluster (0) with four effects: off, fixed (ID `0001`), breathing (`000A`) and cycle
(`0003`); cluster `FF` addresses the effects that span the whole mouse. Sent with the last
byte `00` instead of `01`, the commands are answered but the mouse doesn't change.
`tools\hidpp-probe.exe` lists a mouse's HID++ features and effects; `--test` plays wave,
breathing, cycle and fixed with G HUB's exact bytes.

**Every LED its own color (per-key lighting, `0x8081`).** Switching the mouse to its
whole-mouse effect with ID `0013` (cluster `FF`, effect 4 on the G502 X Plus) makes it
show per-key frames: function 1 with up to four `<zone> R G B` entries, then function 7
(`00 00 00 00`) to show the frame. Its info `00 00 FF 01` lists zones 0–8; a report that
names any other zone is refused whole. Mapped with `hidpp-probe --map`, the zones along the
strip are `3 4 8 7 6 5 2 1`: the six LEDs along the bottom from the thumb side, then the
two up the right side. Zone 0 lights nothing.

**A game's lighting can't be read back from the mouse.** `hidpp-probe --listen` logs every
report the mouse sends (Windows gives each program that has its HID++ collection open a
copy). During a Battlefield 2042 session, G HUB lit the mouse with per-key range writes
(`0x8081` function 5, then function 7 to end the frame) and `0x8071` function 1, but the
mouse only acknowledges them (`01` or `00`) and never repeats the colors. Neither feature
can report the colors currently shown. So a game's LIGHTSYNC on the mouse can't be copied to
other devices this way.

**In LumaBridge:** for breathing, color cycle and the rainbow wave (the full, vivid rainbow),
LumaBridge finds the mouse over HID++ and sets its own effect, with the period taken from
the effect's speed (1–20 s). It finds the effects in cluster 0 by their IDs; the whole-mouse
color wave is used only on the G502 X Plus, where it's confirmed. G HUB's LED SDK stays
connected meanwhile, so G HUB doesn't put its own lighting back. Every other effect
(the other rainbows, gradients, comets, twinkle, static colors and games) is drawn LED by
LED along the G502 X Plus's strip, up to about 20 frames a second. Other Logitech mice get
one color through the SDK as before; handing the lights back
to G HUB brings G HUB's lighting back. Code:
[`logitech_hidpp.h`](../src/app/peripherals/logitech_hidpp.h) (packets, tested against the
capture) and [`logitech_output.cpp`](../src/app/peripherals/logitech_output.cpp).

## ASUS ROG Azoth (wired or wireless, experimental)

Captured from Armoury Crate with USBPcap on a wired Azoth (USB `0B05:1A83`, firmware
rev 0418). The vendor interface is `MI_01`, usage page `0xFF00`, 64-byte reports with
report ID 0, and the commands go on interrupt OUT endpoint 2.

| Command (data bytes) | Meaning |
|---|---|
| `51 2C 00 00 FF <bright> 00 FF FF <R> <G> <B>` | Static color. Brightness `0x00`–`0x64`. Seen for white `FF FF FF`, red `FF 00 2C` and blue `00 03 FF`, at brightness `0x32` and `0x64`. |
| `51 2C <mode> <n> <speed> <bright> <flag> <data…>` | Any effect (below). `<n>` counts the packets still to come, down to `00`. |
| `51 2C <mode> <n-1> <data…>` | The effect's continuation packets. |
| `50 55` | Save to the keyboard's flash (Armoury Crate sends it after every change). **LumaBridge never sends it.** |
| `12 xx`, `22 xx`, `7D 20 02` | Armoury Crate's periodic status queries. |

### Armoury Crate's effects

These were captured by selecting every Armoury Crate effect in turn. An effect's data starts at
byte 7 of the first packet, which holds 11 bytes. It continues at byte 4 of each continuation
packet, which holds 16 bytes. `<flag>` was `00` for one color, `01` for random colors and `10`
for breathing between two colors. `<speed>` was `FF` for static and `07`–`64` for the others;
the capture doesn't show which way is faster.

| Mode | Effect | Data | Packets |
|---|---|---|---|
| `00` | Static | `FF FF <RGB>` | 1 |
| `01` | Breathing | `FF FF <RGB>`, or `FF FF <RGB> <RGB2>` with flag `10` | 1 |
| `02` | Color cycle | `FF FF` | 1 |
| `03` | Reactive | `FF FF <RGB>` (flag `01`: random) | 1 |
| `04` | Wave | `00 02 07` + 7 gradient stops | 3 |
| `05` | Ripple | `FF 02 07` + the same 7 stops (flag `01`) | 3 |
| `06` | Starry night | `FF FF <RGB>` (flag `01`) | 1 |
| `07` | Quicksand | `02 FF` + 6 colors (flag `01`) | 2 |
| `08` | Current | `FF FF <RGB>` (flag `01` or `00`) | 1 |
| `09` | Rain drop | `FF FF <RGB>` (flag `01`) | 1 |

A gradient stop is `<position 0–100> <R> <G> <B>`. Armoury Crate's rainbow is
`0E F5 00 FF`, `1D 00 06 FF`, `2B 00 FA FF`, `39 01 FF 00`, `48 FF F6 00`, `56 FF 78 07`,
`64 FF 00 0D`: purple, blue, cyan, green, yellow, orange, red. The quicksand colors are
the same hues without positions.

What the `00 02` / `FF 02` before the stop count means (direction or width, perhaps) isn't
known yet. The `12 01` query answers `12 01 00 00 00 5C 03 00 01 14 5C E9 10`, which looks
like firmware information, and `12 03` answers zeros.

[`azoth_protocol.h`](../src/app/peripherals/azoth_protocol.h) builds all of these, and the
tests check them against the capture. The app doesn't use the effect modes yet.

LumaBridge sends only the static-color command, at most about 10 times a second, with its own
brightness applied to the color. Because it never saves, the lighting Armoury Crate stored
in the keyboard is untouched and comes back when the keyboard restarts.

### Per-key colors

Not in Armoury Crate's captures, but the command ASUS ROG keyboards take works on the wired
Azoth: `C0 81 <n> 00`, then `n` (up to 15) × `<LED number> <R> <G> <B>`. Tested with
`tools\azoth-probe.exe`, which also maps LED numbers to keys by lighting one at a time and
recording the key you press. On an ISO / Nordic Azoth the LED number is **column × 8 + row**:
row 0 is the F-row and row 5 the bottom row, columns left to right. 82 keys; the tall Enter
is 107. The table is [`azoth_layout.h`](../src/app/peripherals/azoth_layout.h).

LumaBridge draws every effect across the keys, sending only the keys that changed: up to
about 25 times a second by cable, and about 10 through the Omni receiver (report ID 2, as for
the effects), to spare the 2.4 GHz link and the battery.

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

## Everything else: OpenRGB (optional)

Devices LumaBridge doesn't light itself can follow it through OpenRGB, when OpenRGB runs with
its SDK server on (OpenRGB's SDK Server tab > Start Server; port 6742 by default, changeable
on the Integrations page). LumaBridge is a client of OpenRGB's documented network protocol
(`src/app/peripherals/openrgb_protocol.h`): it asks for protocol version 1, lists the
devices, puts each one in its direct mode and sends every LED's color (~30 times a second,
only when something changed; each zone shows the effect along its LEDs). When LumaBridge
lets go (Stop, a game handing back, the device switched off), each device gets its original
mode back, byte for byte as OpenRGB described it.

Devices LumaBridge already lights itself are off in OpenRGB by default, so the two never
fight: ASUS Aura (motherboard and LED strips), Logitech, the ROG Azoth and Kingston FURY /
HyperX memory. Each OpenRGB device has its own page (Devices) to switch it on or off, and all
of them share the "Other devices" lighting.

Tested against a stand-in server built on the openrgb-python library's encoder (the
controller description and the mode sent back matched byte for byte); not yet with OpenRGB
itself. Note: openrgb-python's own encoder pads matrix zones (native struct alignment), so it
isn't a reference for those; OpenRGB writes them unpadded.

## Recognised, not lit natively

The setup guide and the Devices page name RGB hardware LumaBridge doesn't light itself
(`src/app/device_catalog.h`): USB vendors that make RGB gear almost only (Razer,
SteelSeries, Corsair, NZXT, Cooler Master, Thermaltake, MSI, Keychron, Alienware) and
Gigabyte's RGB Fusion controllers by product, plus RGB memory by part number (Corsair
Vengeance RGB / Dominator Platinum RGB, G.Skill Trident Z RGB / Neo / Royal / Z5). Vendors
whose IDs also cover everyday devices (USB sticks, printers, card readers, generic chips)
are left out, so nothing is named wrongly. These light through OpenRGB.
