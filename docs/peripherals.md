# Keyboards, mice and RAM

**Zones (Lighting page):** a zone is a group of devices (`Zone` in `lighting_zones.h`, saved
in the INI's `[Zones]`) with a lighting of its own: manual (always its look) or auto (a game's
lighting while one runs, else its look, a rainbow or dark), its own setup effect and order, its
own travel across its devices, its own brightness. A device is in one zone at most; the devices
in none are Everything, which uses the main lighting and the mode in the header.
`Controller::ApplyZones` puts each zone's lighting on its devices on top of what Decide
chose (`Output::devices`, `Output::gameFor` for the devices showing a game), so every output
shows zones with no code of its own. Setup effects and travel work inside a zone: its parts
(`Controller::setupParts(zoneId)`) are the zone's lit devices in the order of how they sit in
Your setup. Devices that had a look of their own before zones became a zone of one when the
settings were first read. Brightness is the device's times the zone's times the overall
(`Controller::Level`). The idle choice "Armoury Crate" releases devices outside zones;
active zones keep their lighting. Explicit Stop lighting releases every zone too.

**Order follows the arrangement:** the order travel and setup effects use comes from where the
devices sit in Your setup: the flat layout in 2D, the desk and the case in 3D
(`Controller::ArrangedSpot`). The Nollie hub itself isn't in Your setup (it has no lights); its
fans and strips count as the Nollie fans, in the case.

**What's plugged in?:** things LumaBridge reads from USB it can only use well when told what is
connected: a Nollie hub (what is on each channel) and the motherboard's ARGB header (how many
fans). The Lighting page asks once, with "Light it" to find the right header; "Ask me later"
hides it for the session, "Done" for good (`[Nollie] Asked`, `[App] FansAsked`).

**Setup effects (all outputs):** `fx::Kind::FillOneByOne` and the kinds after it are effects
across the whole setup: every part gets its place (`Params::part` of `Params::parts`) and
`fx::Render` draws the part's LEDs from it, so every output shows them with no code of its own.
`Controller::UpdateSetupParts` lists the lit devices, how many parts each has (each fan of the
Aura header when each fan shows the pattern, each Nollie fan on its own and each strip, each
memory stick (or all sticks as one: `[Effects] RamSticksApart=0`), one for the board, keyboard, mouse, controller and the other devices), and puts
them in order by where they sit in Your setup (left to right by default). `RenderFans`, the
Nollie plans and the RAM renderer give each of their parts its own number. They replace your
lighting (and devices' own looks) while chosen; games still light everything at once; Nollie
sequences and travel step aside; a Nollie gradient colors them.

Across the setup also offers Fill to solid/pulse, Center to solid/pulse and Ignite to
solid/pulse. Except for ongoing Center to pulse (which contracts and repeats), these play
their reveal once, then hold the main color or pulse together on a
four-second cycle between 44% and full intensity over the second color. Speed controls the
reveal, while the final pulse stays gentle. Gallery previews repeat the reveal for browsing.

**DualSense moving effects:** the lightbar is one color and the five player LEDs are on or off
(`dualsense_effects.h`). For moving effects the lightbar shows the average color of the effect
across the controller (a comet pulses as it passes, a fill brightens as it fills), or its first
LED where the hue is what moves (rainbows, flowing gradients); the player LEDs show the motion: a
comet chases across them, a fill fills them from the left, sparkles twinkle, a strobe blinks them
all, breathing is a level meter. Still colors and rainbows leave them off. The report bytes are
the ones Linux's hid-playstation driver documents (player LEDs in the common output block: USB
byte 44, Bluetooth byte 46, five bits). Toggle: Devices > DualSense, "Show moving effects on the
player LEDs" (`[App] DualsensePlayerLeds`). Not confirmed on a real controller: which end is
LED 1, and whether Bluetooth takes the player bits as written.

**Keep-alive (all outputs):** outputs only send frames that changed, and the same frame again
every 1 to 2 seconds (RAM, the Logitech mouse, the DualSense and the Kraken every second; the
Azoth, Windows lighting devices, OpenRGB and the Nollie hubs every 2 s; the Aura every second).
Devices that go back to their own effect when they hear nothing for a while (the sticks'
rainbow, the G502's color cycle) would otherwise show it during a still stretch of an effect,
such as the dark parts of a bounce; it used to be 3 to 5 seconds.

**Frame timing (all outputs):** every output thread paces its frames with `FramePacer`
(`src/core/frame_clock.h`): a high-resolution waitable timer on a steady beat, and effect time
from `PreciseNowMs` (the performance counter on GetTickCount64's scale) through
`fx::SecondsAt`. `Sleep` and GetTickCount64 alone move in ~16 ms steps, which made 30 frames a
second really 21 to 32, and effects jump. Rates: Aura up to `[Aura] MaxUpdateHz` (default 60
for new settings; Settings > Maximum update rate), Nollie / LampArray / OpenRGB 60, Logitech 50,
Azoth and DualSense 30 (the Azoth 20 through the Omni receiver, on its own beat), RAM 30, Kraken
10. The Aura thread waits on the timer together with its wake event while pumping messages (it
is an STA). The hardware helper looks for a new RAM frame every 5 ms (it polled every "30 ms",
really 31 or 47, which made the app's 20 frames a second land unevenly). It writes every
register every frame: tried writing only the changed ones, and the sticks then fell back to
their own rainbow until the next whole frame. The helper log says how long a frame takes on the
bus ("RAM: a frame (... writes) takes ... ms"), which is what limits the memory's rate.

## Logitech (G502 X Plus and other Logitech RGB gear)

**Without G HUB:** LumaBridge tries direct HID++ lighting even when Logitech's LED SDK
is missing or cannot initialize. The G502 X Plus keeps its per-LED effects and game
lighting; other mice exposing the supported RGB effects protocol can follow as one
color when a fixed-color effect is available. Only one directly reachable mouse is
controlled by this output. Unsupported Logitech keyboards and other devices can use
the Windows HID LampArray connection, if their firmware provides it, or the optional
OpenRGB connection. OpenRGB must be running with its SDK server enabled.

G HUB remains optional for supported direct mice and required for the SDK output to
other Logitech gear. LumaBridge retries SDK initialization while running. A game only
gets exclusive Logitech lighting when the SDK connection is active; without it,
LumaBridge keeps drawing its game lighting directly. With no vendor app to take over,
releasing direct control stops writes and can leave the last color on the device.

LumaBridge loads G HUB's own LED SDK DLL (`LGHUB\sdks\sdk_legacy_led_x64.dll`, found
through the machine-wide `ServerBinary` registration and never LumaBridge's own proxy) in
the app's process when available. It calls `LogiLedInitWithName("LumaBridge")`,
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

**Sleep.** LumaBridge used to keep sending (frames ~20 times a second while an effect
moves, the same frame every 5 s otherwise), which kept a wireless mouse awake: its lighting
never faded and it never slept. Now it watches when the mouse is used (raw input from the
Logitech receiver or cable, `src/app/device_sleep.h`): after the chosen time without use
(1 minute by default, on the device's page) its lighting fades out over 2.5 s, then nothing
more is sent until it's used again, so the mouse can sleep; the first movement lights it
again. Whether it fades follows G HUB's "turn off lighting on inactivity"
(`/lighting/turn_off_for_inactivity` over G HUB's local WebSocket), read in the background;
without an answer from G HUB it fades. The ROG Azoth does the same (5 minutes by default).
"Refuse to sleep while a dynamic effect is showing" (off by default, on the device's page)
skips the timeout entirely while its current effect isn't a plain static color (a game, or an
animated preset like Rainbow or Comet), so it doesn't fade out mid-effect; a still color still
sleeps as usual.

## ASUS ROG Azoth (wired or wireless)

**Devices > ROG Azoth > OLED display** offers screen on/off,
brightness, six built-in animations and a 12/24-hour clock, independent of RGB control.
The default leaves the screen unchanged. With Armoury Crate, it can open the OLED setup
workflow and export a 256 x 64 banner BMP for manual upload. Direct LumaBridge GIF uploads
and the CS2 bomb countdown have been confirmed working on the owner's Azoth over wired USB.
USB power and brightness acknowledgments were also confirmed; live music modes still need
physical-screen validation.
See [the OLED investigation](azoth-oled-research.md).

The status checks present USB interfaces every second and prefers the cable over Omni.
An attached receiver alone does not mean the keyboard is connected: wireless control
requires a valid keyboard power reply. These queries pause during keyboard sleep.

LumaBridge talks to the keyboard directly over raw HID; **Armoury Crate isn't needed** to
run it, installed or otherwise. Detection (`setup_plan.h`, `azoth_output.cpp`) matches only
the two product IDs below, found on one wired unit - a different region, SKU or firmware
revision reporting a different PID won't be found (the Azoth card on its Devices page shows
"Not found" with a diagnostic in that case; run `tools\device-probe.exe` to see what
LumaBridge actually sees).

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
the effects), to spare the 2.4 GHz link and the battery. By cable, each report waits for
the keyboard's answer (at most 20 ms) before the next goes out, as OpenRGB does for ASUS
keyboards: sent back to back, they locked up the lighting interface within a second of
plugging in (a write failed with Windows error 31, and only a replug brought it back).

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

## Sony DualSense (PS5 controller), lightbar only

USB lighting (`054C:0CE6`, or `054C:0DF2` for DualSense Edge) is confirmed working on real
hardware. Bluetooth lightbar changes are also confirmed on the owner's controller using
Windows' 547-byte HID collection. No Steam or other software is required. The lightbar shows one color from
LumaBridge's effect; the five player indicators stay off.

The USB packet keeps the previously verified OpenRGB-derived layout. Bluetooth follows the
[Linux PlayStation driver's documented report fields](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c)
and [SDL's Bluetooth header correction](https://github.com/libsdl-org/SDL/commit/129627068fe266ed68c0725f47a026eae889b992).
Its 78-byte report starts with `0x31`, a four-bit sequence in the high nibble, and `0x10`.
The common payload starts at byte 3; RGB is at bytes 47–49. Bytes 74–77 contain the
little-endian CRC-32 covering `0xA2` plus report bytes 0–73.

Windows may report a maximum output size of 547 bytes for the Bluetooth collection, even
though the lightbar packet itself is 78 bytes. Detection uses the gamepad's input report
size, and output is zero-padded to the HID collection's maximum size, following
[HIDAPI's Windows write behavior](https://github.com/libusb/hidapi/blob/master/windows/hid.c).

On Bluetooth connection, LumaBridge requests feature `0x09` for enhanced reports, then sends
one lightbar setup/reset packet with SDL's LED-release flag before the first color, after a
five-second startup wait so Sony's connection animation can finish. The reset is repeated after handing
lighting back and reclaiming it. Color packets enable only lightbar and player indicator
updates; they leave audio, microphone, rumble and trigger controls alone. Tests check header,
sequence wrap, startup fields, output offsets and a checksum fixture computed independently.

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
  switched on. Each address gets up to three tries, since another program on the bus can
  make a chip miss one. When fewer SPD chips answer than the firmware lists sticks (seen:
  one of three on one start, all three on the next), all four slots get the colors: the
  controller covers every slot, and an empty slot's registers are only written.
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

## Any brand: Windows' lighting standard (HID LampArray)

Devices that implement HID LampArray in their firmware (the standard Windows 11's Dynamic
Lighting uses: usage page 0x59, "Lighting And Illumination") are lit directly, with nothing
installed (`src/app/peripherals/lamparray_output.cpp`). LumaBridge finds every HID interface
whose top-level collection is a LampArray, reads the array's attributes (lamp count, kind,
minimum update interval) and each lamp's attributes (position, color levels), turns off the
device's own effect (AutonomousMode = 0) and sends every lamp's color in multi-update reports
(as many lamps per report as the device's descriptor allows; the last one of a frame marked
complete). Effects run across a device left to right, by the lamps' X positions. When
LumaBridge lets go, AutonomousMode goes back to 1 and the device runs its own effect again.

The reports are addressed by usage through Windows' HidP_* functions, so the layout comes
from each device's own descriptor. Devices LumaBridge lights another way (the ROG Azoth,
Logitech through G HUB, the Aura controller) are off here by default. If Windows' own
Dynamic Lighting also controls a device, the two take turns: turn it off for that device
(Settings > Personalization > Dynamic Lighting). Not yet tried on a real LampArray device.

## Nollie ARGB hubs

Nollie 1, 8, 16 and 32 channel hubs (and the 28 series and Prism8) are lit over USB HID, with no
other program: `nollie_protocol.h` (reports and device table, pure and tested),
`nollie_layout.h` (what is on each channel), `nollie_output.cpp` (the thread). The protocol and
the table of USB IDs, channel counts and wiring follow OpenRGB's Nollie driver
(`Controllers/NollieController`); LumaBridge only sends colors.

**What the hub keeps on its own (read only).** The 64-byte hubs with the OS2 firmware (USB
vendor 16D5) play effects with no program running, and a boot effect at power-on, set up in
Nollie's own app. When such a hub is opened, LumaBridge asks it once what it keeps and writes
the answers to the log ("Nollie: ... firmware", "at power-on", "boot canvases", "canvas 14
(boot)"): `nollie_onboard.h` (the questions and how the answers are read, pure and tested),
`LogOnboard` in `nollie_output.cpp`. The questions are the ones Nollie's app sends (`F9` and
`FC` reports); nothing is written. The 8 channel hub has no storage for animations: its boot
effect is one of eight built into the firmware (Light up, Color wipe, Slowly wipe, Pull flower,
Stackab1-3, Bullets) with colors and a speed, on up to two boot canvases (14 and 15). The
answers' layout is not confirmed on real hardware yet; each log line also carries the raw
bytes. A hub that doesn't answer two questions in a row is not asked again.

- **16 / 32 channel hubs:** one 1025-byte report per channel (channel, flag, LED count
  big-endian, then G R B per LED). Channels go out in hardware order; channels 15 and 31 always
  go out (they carry the flag that ends a bank), with 8 ms after each. The zone numbers the user
  sees map to hardware channels through a per-model table.
- **Other hubs:** 64-byte reports of at most 21 LEDs (R G B, or G R B on some models), numbered
  `packet + channel * interval`, then one update report (`0xFF`).
- **Settings the hub is told:** the 1-channel hub gets its LED count (`0xFE 0x03 ...`); the
  original 32-channel hub gets its power stage switched (`0x80 n`) when channel 27 is longer
  than 20 LEDs.

Where things are: the hub's page on Devices has its switch and the channel setup (what is
plugged in, LED counts, names, the test pattern). On the Lighting page the hubs are one more
device, "Nollie fans" (`device::kNollie`), next to the fans, motherboard and memory: they follow
the main lighting, or a look of their own, with their own brightness and direction, appear in
"Your setup" and take part in the flow across the setup (where the case's fans are). Picking
them (Manual mode; at the end of the page in Auto mode) shows what only Nollie hubs can do,
under "Only on Nollie hubs": travelling effects, sequences, gradients, the refresh rate, the
and each channel's or fan's own lighting (a channel without one shows the Nollie
fans' look); the boot effect is on the hub's page under Devices. Colors go through the same calibration as the Aura fans (Settings), so the same
color looks the same on both.

A hub can't say what is plugged in, so the Devices page asks per channel: not used, an LED
strip (its length), or fans (how many, LEDs per fan, one pattern per fan or across them all).
A hub with no channel set up is left alone, not even blanked. Each channel can have its own
lighting, brightness and direction like any other device, be handed back, and show a test
pattern (each fan one color with a white first LED). The settings are in the INI's `[Nollie]`
section. When LumaBridge lets go it stops sending: the hub keeps its last colors until it
loses power. Close Nollie's own app and SignalRGB first; only one program can hold the hub.
Devices OpenRGB also lists as Nollie are off in OpenRGB by default while this output works.

**Fans on their own:** a channel of chained fans can show one pattern across all of them, or
(the default) each fan on its own: every fan renders the effect around its own ring
(`RenderSegments`), can have its own look and brightness (`Look.<channel>.f<n>` in the INI;
without one it shows the channel's), and is its own step when an effect travels.

**Travelling effects** (their own section on the hub page): per hub, the effect can run through
the parts in use (channels in order, each fan on its own a step, first to last or the reverse),
fan by fan or smoothly LED by LED (`nollie::Place`: each part's first LED gets its place along
the way through `Flowed`, and its LEDs run behind it in proportion, `Segment::lag`, so the
effect flows from one fan into the next), over a set number of seconds, the same way the Lighting
page's flow moves an effect across the setup (`Flowed`, `device_lighting.h`). Games still light
every channel at once.

**Sequences** (in Travelling effects): effects that know the order of the hub's parts
(`nollie_sequence.h`), shown instead of each channel's lighting, in the travel's order: fill
each fan around its ring one after another until all are lit, then dark and again; fill then
empty; one fan at a time; rainbow fan by fan; a comet bouncing through all fans; dots stacking
up. Each has a lit and an unlit color and a time per fan; channel brightness and direction
apply; games still light every channel at once. A sequence starts over when it is changed or
with "Start over".

**Gradients:** a gradient of 2 to 6 colors runs along the parts in channel order and takes the
place of the effect's main color LED by LED (`nollie::GradientAt`, `GradientPlace`): a still
gradient, breathing or strobing in it, a comet changing color as it goes, sequences lit in it.
It can move (loops back to its first color). Rainbows and games keep their own colors.

**Refresh and timing:** frames go out at 30 or 60 Hz per hub (60 by default), on a steady
beat from a high-resolution waitable timer (Windows 10 1803 and later; older Windows falls
back to the system timer's steps, logged at start). Effect time comes from the performance
counter aligned to GetTickCount64 (the effects' clock), so frames land to a fraction of a
millisecond instead of GetTickCount64's ~16 ms steps. Only changed frames are sent; the hub
page shows the frames actually sent per second. 16 / 32 channel hubs pause 8 ms after each
flag channel per frame, so they can stay a little under 60.

**Boot effect** (on the hub's page under Devices, not on the Lighting page): the hub sits on an internal USB header and is dark until a program sends it
colors, and no way to save an effect in its firmware is documented, so LumaBridge plays the
effect itself: when it starts lighting the hub (Windows start, if LumaBridge starts with
Windows; wake from sleep; the hub plugged in again), for a set number of seconds, with its own
look; it follows the travelling effect's order. The hub page has "Play it now". Only hubs with a
channel set up play it. The boot editor also offers the six settling reveals above, ordered
across the hub's configured fans and strips. The reveal occupies 60% of the selected boot
duration, followed by its solid or pulsing finish. The final 20% (up to 0.8 seconds) blends
into the current normal lighting, including sequences and gradients. Boot colors remain
independent of the normal gradient.

Confirmed on the owner's hub (on an internal USB header, three fans and an LED strip): the
channels, fans on their own, the travelling effects and the sequences. Not checked on other
models: which USB interface carries the reports on hubs that list several (the log lines
"Nollie: ... opened" show the interface and report size found).

## OpenRGB (only if it's already on the PC)

LumaBridge never needs OpenRGB, and the connection is off unless OpenRGB is installed or
running (the setup guide checks). When it's on, devices LumaBridge doesn't light itself can
follow it through OpenRGB, while OpenRGB runs with
its SDK server on (OpenRGB's SDK Server tab > Start Server; port 6742 by default, changeable
on the Integrations page). LumaBridge is a client of OpenRGB's documented network protocol
(`src/app/peripherals/openrgb_protocol.h`): it asks for protocol version 1, lists the
devices, puts each one in its direct mode and sends every LED's color (up to 60 times a second,
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

(Nollie hubs are lit natively; see above. They are still in the catalogue so they can be named.)

The setup guide and the Devices page name RGB hardware LumaBridge doesn't light itself
(`src/app/device_catalog.h`): USB vendors that make RGB gear almost only (Razer,
SteelSeries, Corsair, NZXT, Cooler Master, Thermaltake, MSI, Keychron, Alienware) and
Gigabyte's RGB Fusion controllers by product, plus RGB memory by part number (Corsair
Vengeance RGB / Dominator Platinum RGB, G.Skill Trident Z RGB / Neo / Royal / Z5). Vendors
whose IDs also cover everyday devices (USB sticks, printers, card readers, generic chips)
are left out, so nothing is named wrongly. A brand whose device has Windows' lighting
standard is lit directly instead of named here; the rest need their own app (or OpenRGB).

## Final color when releasing lighting

Settings > When LumaBridge lets go enables an optional solid handoff color. Closing the
app or choosing Stop lighting sends that color to enabled outputs and configured Nollie
channels, preserving brightness and native exclusions. Boot effects, Nollie sequences,
gradients, device sleep and controller indicator animations step aside for the final frame.
The default remains restoration of the usual device/app lighting. With a custom final
color enabled, LumaBridge skips the Aura restart that would immediately replace it; RAM
uses its keep-last-color release mode, OpenRGB keeps its direct frame and LampArray avoids
an autonomous-mode reset.

A companion process waits using inherited parent-process and clean-exit event handles.
After unexpected parent termination it checks the single-instance mutex before loading
fresh preferences and sending the color for five seconds. It exits without changing the
user's saved effects or clearing the crash-loop flag. Recovery has a twenty-second process cap.
Clean exits signal intent before worker cleanup, so cleanup delays do not trigger recovery. Restarting the app cancels recovery and waits for it to release ownership. Killing the whole process tree, powering down Windows, device
resets, and other lighting software taking control can prevent or overwrite the color.
Some SDKs (including Logitech's) restore their own profile on disconnect, so a final frame
cannot guarantee that another app will keep it. No firmware settings are changed.

DualSense player indicators have an independent pattern, speed, five-bit light selection
and a per-effect enable mask. Turning them off or excluding an effect sends zero player
bits while the RGB lightbar continues normally. Preferences are stored under [Controller]
(PlayerMode, PlayerSpeed, PlayerMask, PlayerEffects), alongside the existing global
[App] DualsensePlayerLeds switch.

RAM updates now cover all four documented RGB register groups even after a complete SPD
scan; SPD numbering alone is not used to exclude RGB groups. The app produces frames at
30 Hz, while the helper's actual rate remains limited by SMBus transactions. New helpers
advertise coherent frame publication (odd/even sequence) and report successful frame count
and transaction duration in shared memory. The first transaction and every 200th are timed
in helper.log. Failed transfers stop before applying a partial frame and are retried without
consuming the app's frame sequence. Upgrade both the test app and hardware helper for these
changes; an already running helper continues using its loaded executable until it exits.

Center to pulse in ongoing lighting expands from the center, completes one four-second
breath, contracts back to the center at the reveal speed, rests dark for one second and
repeats. Boot parameters set `oneShot`, preserving the reveal-and-pulse finish and the
existing smooth fade into normal lighting. The playback flag participates in parameter
comparison and is derived by the boot planner rather than stored as a user setting.

## Release stabilization checks

Hardware helper task version 8 is required for coherent RAM frame acknowledgements and
complete RGB group coverage. The app and helper use one version constant; a Windows test
checks that the elevated installer agrees. The Memory page reports measured bus transaction
time and delivered-frame count, rather than presenting its producer rate as measured FPS.

Final-color requests carry a delivery token. Successful output writes acknowledge it;
RAM acknowledges only after the SMBus transaction succeeds. Handoff waits within a deadline
and logs missing acknowledgements. Bluetooth recovery allows its five-second connection
handshake before requiring delivery. Individual native handbacks use the same final-color
path. Another app or firmware can still overwrite a successfully delivered frame.

Auto zones can request screen capture while Everything is Manual. Idle handback for
Everything no longer disables active zones; devices outside zones are released. Explicit
Stop lighting and the crash guard still stop every zone. Aura's board and headers share
one ownership boundary with Armoury Crate; their native handback remains paired.

Numeric settings reject nonfinite, out-of-range integer and trailing-junk input. Effect
metadata is shared by the regular and boot editors, and unknown IDs use a safe fallback.
Duration-driven boot reveals hide the ignored speed slider; their preview uses the same
one-shot/duration planner as the output.

`test_windows` exercises real GameSense client/listener shutdown, CS2 listener restart,
INI persistence and synchronous I/O cancellation without device writes. `test_helper_version`
checks upgrade consistency. `tools/dev/check-lifecycle.py` checks full-app clean exit,
restart, forced-exit recovery and restart during recovery in an isolated Wine prefix.
The older terminate-only failure did not include a usable stack; abort/terminate now records
exception details, stack offsets and a minidump. Its exact historical cause is unconfirmed.
