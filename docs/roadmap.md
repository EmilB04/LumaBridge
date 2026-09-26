# Roadmap

## Verify on hardware (next)

1. `aura-test.exe list` → record devices/matrix sizes in `aura-sdk-notes.md`. Confirm the
   Azoth shows up as an Aura keyboard.
2. `aura-test.exe cycle` → every selected device changes color and Armoury Crate takes
   over again afterwards.
3. `logiled-harness.exe <G HUB dll>` then `logiled-harness.exe LumaBridge_x64.dll` →
   the mouse behaves identically in both runs, and Aura follows in the second.
4. Process Monitor on BF1 (see `logiled-exports.md`), install, play, read the log.
5. A Chroma game with the emulator.

## Milestone 4: per-key (Azoth)

- Keep a full frame instead of one color: LogiLed 21×6 bitmap and per-key calls
  (`KeyName`/scan code → cell), Chroma 6×22 grid (cells map to `ChromaSDK::Keyboard::RZKEY`
  row/col).
- Aura side: `IAuraSyncKeyboard::Key(scanCode).Color` for keyboards, falling back to
  `Lights(i)` by index. A static table maps grid cell → Aura key code for the Azoth's
  75% layout. Cells the Azoth lacks (numpad, F13+) are dropped.
- Other Aura devices keep getting the ambient reduction of the same frame.
- The mirror worker grows a "frame" path: one `Apply()` per device per frame, still
  coalesced and rate-limited.

## Milestone 5: config / tray

- Tray app (`src/tray-app`): live ambient color preview, enable/disable per device,
  brightness and gain sliders that write `LumaBridge.ini`, "open log", install/uninstall
  buttons wrapping the PowerShell scripts, autostart.
- The DLLs re-read the ini when it changes (`FindFirstChangeNotification` on the worker
  thread).

## Later

- **Chroma reader front-end** for signature-checking games (genuine Razer runtime +
  read its effect stream, Artemis-style).
- **Corsair iCUE SDK 3.x emulator** (see `chroma-emulation.md` for the trade-offs).
- Single Aura owner across processes / front-ends (a small named-pipe service) so two
  games, or LogiLed + Chroma in one game, don't fight over `SwitchMode`.
