# ROG Azoth OLED investigation

Investigated 2026-10-01. Scope: the original ROG Azoth already supported by LumaBridge,
not the colour-screen Azoth Extreme. The 2026-10-02 test build implements the basic
commands below, with opt-in ownership and reply checking. On 2026-10-02, USB status,
animation-read, screen-enable and brightness exchanges were confirmed on the owner's
original Azoth (USB device version 0418). Animation/clock visuals and hotplug still need validation.

## Feasibility

Basic display control can use the keyboard's vendor HID connection without Armoury
Crate. G-Helper's maintainer lists Azoth OLED support in the
[keyboard support announcement](https://github.com/seerge/g-helper/discussions/5710).
The existing LumaBridge lighting transport already opens the corresponding wired and
Omni interfaces. The test build serializes lighting and OLED requests on that worker.

ASUS specifies **256 x 64 pixels** for the original Azoth, with imported animations
limited to 196 frames and 25 fps. The Extreme uses a different resolution. These are
Armoury Crate import limits, not proof of a USB streaming format.
[ASUS OLED specifications](https://rog-forum.asus.com/t5/technologies-explained/rog-azoth-azoth-extreme-oled-customization-upload-your-own-image/ba-p/1063996).

## Commands found in an existing implementation

The firmware idle timer is separate from RGB/OLED writes. G-Helper's
[AsusKeyboard energy settings](https://github.com/seerge/g-helper/blob/main/app/Peripherals/Keyboard/AsusKeyboard.cs)
reads it from byte 7 of `ID 12 01` and writes `ID 51 38 00 00 value`. Values 0..4 mean
1, 2, 3, 5 and 10 minutes; FF disables firmware sleep. LumaBridge remembers the
reported ASUS value, uses FF while controlling the awake keyboard, explicitly darkens
RGB and OLED at LumaBridge's timeout, and restores the ASUS value on sleep or release.
It never writes the low-battery setting or sends the flash-save command. Timer commands
require matching acknowledgments. The timer override and RGB/OLED inactivity behavior
were verified on the original ROG Azoth.

[G-Helper's Azoth model](https://github.com/seerge/g-helper/blob/main/app/Peripherals/Keyboard/Models/Azoth.cs)
uses these reports, padded with zeroes to the link's report length. `ID` is the HID
report ID, included in the Windows buffer: `00` wired, `02` for Omni. Wired buffers
are 65 bytes; Omni buffers are 64 bytes.

| Operation | Buffer prefix, hex unless marked otherwise |
|---|---|
| Display enabled | `ID 69 00 00 00 enabled`, enabled = 0 or 1 |
| Brightness | `ID 68 00 00 00 brightness`, brightness = decimal 0..100 |
| Built-in animation | `ID 61 00 00 00 index`, index = 0..5 |
| Read selected animation | `ID 21 00`; upstream reads reply byte 5 |
| Clock | `ID 63 00 00 00 format yearLo yearHi month day hour minute` |

Upstream notes brightness rounding in steps of 25. Clock format 0 selects 24-hour
time; 1 and 2 represent AM and PM. Clock updates are sent each minute. Treat these
as candidates requiring confirmation on the owner's firmware. Their persistence
and reliable restoration of the previous screen are not established here.

The upstream [keyboard transport](https://github.com/seerge/g-helper/blob/main/app/Peripherals/Keyboard/AsusKeyboard.cs)
drains queued input before sending a command and waits for a reply. It uses a 300 ms
USB timeout and rejects a reply beginning with `ID FF AA`. Its interface probe queries
`ID 12 00` and checks the reply, rather than accepting a successful write as proof of
device support. LumaBridge's test transport now reads replies with a 300 ms deadline,
matches command bytes and rejects error reports. It verifies the control interface with
`ID 12 00`; the selected-animation read is optional and does not block other OLED controls.

Connection discovery runs every second and on Windows device changes. A wired control
interface takes priority even if an Omni handle is already open. The Omni receiver's
version query answers when the keyboard is offline, so wireless readiness requires
`ID 12 01`, with matching command bytes and a valid battery percentage in reply byte 6.
When awake, this is checked every two seconds. During sleep, only USB descriptors are
queried; receiver presence is reported as unverified rather than a confirmed wireless keyboard.

The owner's USB replies were `00 12 00 ... 18 00 04` (version),
`00 12 01 00 00 00 47 02 01 01 ...` (71%, charging), and `00 21 00 ...`.
While using USB, Omni answered its own 0708 version but rejected keyboard-power and
OLED queries with `02 FF AA`. This explains the previous false wireless status and OLED errors.

## Custom content and live readings

The inspected Azoth implementation provides no custom bitmap/GIF upload or hardware
telemetry commands. Images, text banners, CPU/GPU temperatures and game statistics
therefore need additional protocol research. This is not evidence that they are
impossible. LumaBridge already has CPU/GPU sensor values in `SystemMonitor`; sending
those values to the keyboard is the missing part.

ASUS now lists the original Azoth in
[Gear Link's supported devices](https://www.asus.com/me-en/support/faq/1054795/), with
Companion needed for full functionality and an Omni-spec receiver requirement.
That provides another reference client for later protocol investigation. It does
not establish an app-free custom-content protocol for LumaBridge.

## Methods when Armoury Crate is available

### 1. Let Armoury Crate own the OLED

Use Armoury Crate's Azoth device page to select the OLED's image/animation, clock,
music or hardware-information mode. LumaBridge can continue sending its existing RGB
reports while leaving OLED reports alone. This is the first coexistence mode to test;
independent OLED/RGB ownership has not been verified on the owner's firmware.

For a custom image or GIF, select **Azoth > OLED > Image or Animation**, enable it,
choose **Custom image/animation > Replace File**, then **Apply**. LumaBridge could
prepare the asset and open Armoury Crate using its existing `LaunchArmouryCrate()`;
the user would complete the upload. A validated OLED deep link was not found.
[ASUS upload instructions](https://rog-forum.asus.com/t5/technologies-explained/rog-azoth-azoth-extreme-oled-customization-upload-your-own-image/ba-p/1063996).

This method gives the user ASUS's built-in screen features immediately. It does not
let LumaBridge send its own changing game statistics to the display.

### 2. Use Armoury Crate's installed keyboard service/SDK

Read-only inspection of the installed software found a more capable private interface.
The keyboard module builds XML requests with a keyboard type, model, optional serial,
function number and settings, then submits them through its framework's `send()`.

Installed evidence on this PC:

- `C:\Program Files (x86)\ASUS\ArmouryDevice\modules\keyboard\index.js`
- `C:\Program Files (x86)\ASUS\ArmouryDevice\dll\KeyboardSDK\ArmouryKbSDK.dll`
- `C:\Program Files (x86)\ASUS\ArmouryDevice\resources\app.asar`

| Framework function, decimal | Purpose observed in the keyboard module |
|---|---|
| 100 | Choose a built-in animation |
| 101 | Custom animation, with upload state and an input path |
| 102 | Banner template, direction, font and text |
| 103 | Hardware information choices and update interval |
| 104 / 105 | Brightness / display enabled |
| 108 / 109 | Music information / stop selected OLED update timers |
| 113 | Clock information |

**These are internal framework IDs, not HID command bytes.** This is a shared
keyboard module; its list does not prove every function works on the original Azoth.
Its system-information request selects ASUS sensor sources. Arbitrary LumaBridge
sensor values and custom game telemetry are not demonstrated by that request.

Static export inspection found `Entrypoint`, `ExecuteFunction`, `Post_Entrypoint`,
`Post_EntrypointReturn`, `freeBuffer`, `startService` and `timerCallback` in the
32-bit DLL. The installed framework worker invokes a wrapper's
`Post_EntrypointReturn` and `startService`. Calling conventions, initialization,
device capabilities, request routing and buffer ownership still need validation.
Export names alone are insufficient to safely call the native DLL.

Proposed integration: first reproduce a read-only capabilities query through the
normal installed framework. If direct DLL access is necessary, keep it in a separate
32-bit helper process and return results to the 64-bit LumaBridge app. Detect the
installed version and disable this route if its interface is unsupported. Prefer
the service's existing request queue when reachable, rather than creating a second
hardware owner. No DLL was loaded or service request sent during this investigation.

The inspected keyboard module's SHA-256 is
`5ff6785cd8a8871274cfa577d5dd29972256a3aa9b1a0a7cdb0fe19821fe6d47`.
This identifies the evidence, not a compatibility whitelist.

### 3. Automate Armoury Crate's UI

A possible fallback is Windows UI Automation to select an OLED mode or upload a
prepared file. First verify that the installed device page exposes usable controls.
This is suitable for occasional setup and upload operations; it depends on ASUS's
page structure and is a poor choice for frequent display updates. No automation
has been implemented or tested.

### 4. Use direct HID alongside the installation

The basic commands above do not need Armoury Crate, even when it is installed.
Before letting LumaBridge own the OLED, stop ASUS's OLED update mode through a
validated interface so its timers do not overwrite the display. Snapshot supported
settings first and restore them on release where possible. Simply closing the
Armoury Crate window does not establish that its background OLED updates stopped.

### Backend choice

Recommended default: leave OLED ownership with Armoury Crate when it is available.
Offer the private ASUS interface as an optional backend only after validation, and
direct HID for basic controls when ASUS software is unavailable or the user chooses
it. UI automation can provide a setup fallback. Never let two backends continually
write the OLED at the same time.

ASUS's public [Aura SDK](https://www.asus.com/campaign/aura/us/SDK.php) describes
RGB illumination. No public Azoth OLED API was found in the reviewed documentation;
the installed keyboard SDK above is a separate interface from `aura.sdk.1`.

## Test build and remaining validation

**Devices > ROG Azoth > OLED display (test)** defaults to leaving the display unchanged.
Select direct control for screen on/off, brightness, six animation choices, or a local
12/24-hour clock. "Keep current content" adjusts screen power and brightness without
selecting content. Clock requests occur when the local minute changes; all OLED requests
pause during keyboard sleep. RGB and display ownership are independent, and settings
are saved under `[AzothOLED]` in the existing INI.

The Armoury Crate option stops direct OLED requests. "Open Armoury Crate" also releases
display ownership, then launches the installed app. "Export banner and open folder"
creates `%LOCALAPPDATA%\LumaBridge\AzothOLED\banner.bmp`, a 256 x 64 grayscale image
for Armoury Crate's manual importer. The app does not automatically upload this file,
invoke the undocumented private keyboard SDK, or automate Armoury Crate's UI.

Validate screen controls by cable first, then Omni; test RGB running concurrently,
reconnects, sleep/wake and returning to Armoury Crate. Record firmware and acknowledgments.
Stopping direct control leaves the last display state until the official app applies
another selection; it does not promise restoration of a previously selected screen.

For direct custom content, capture one small image upload and one changing telemetry
value from the official client. Determine framing, pixel/value encoding, acknowledgments,
storage writes and completion before implementing uploads or continuous updates.

## Animation previews and LumaBridge effects (2026-10-02)

The original Azoth's installed Armoury Crate view modules (6787, 6789 and 6791) map
firmware indices 0..5 to `rog_ani_1`, `rog_ani_6`, `firework`, `rog_ani_4`, `heartbeat`
and `rog_ani_8`. The selector now labels these as ASUS built-in effects. The preview
decodes these local GIF assets with Windows Imaging Component, preserving frame delays,
offsets, transparency and disposal. Their 208 x 64 artwork is centered on the 256 x 64
screen; firmware status icons are not simulated. These ASUS files are read from the
installation, not bundled. Missing artwork shows a preset label instead of an unrelated
animation. The index mapping comes from `View/6789/index.js`'s `preloadAnime` list.

The former preview stand-ins are now a separate LumaBridge GIF collection: Wave, Level
bars, Stars, Scanner, Rings and Rain. Their shared renderer creates both the preview
pixels and 256 x 64 grayscale GIF exports, with 150 frames at 20 fps and a 7.5-second
loop. The animation source and each collection's selection are saved independently in
the INI. Choosing a LumaBridge effect does not send `0x61`, select a substitute ASUS
preset or imply that the GIF has been uploaded. Screen power and brightness still work.
Export the GIF, then open Armoury Crate and use Custom image/animation > Replace File >
Apply. The GIF collection uses six animated selection tiles, with export, launch and
copy-path actions together. An illustrated four-step guide covers export, the Azoth OLED
page, Replace File and Apply. The gallery and guide stay visible while Armoury Crate is
open, so the file path can still be copied during import. Direct uploads remain pending
protocol validation.

Later the same day the Armoury Crate / LumaBridge choice was removed: LumaBridge always
runs the screen (power and brightness), and **Keep current** leaves the content to Armoury
Crate's uploads and modes. Opening Armoury Crate no longer releases the screen.

These changes need an independent implementation; upstream source is a reference,
not code imported into LumaBridge. No firmware replacement is needed for the basic
commands above.
