# Roadmap

## Verify on hardware (next)

0. Start `LumaBridge.exe`, switch to **Manual**, and pick a color. Every Aura device
   should follow, and **Devices** should list them. Switch back to Auto: with "Armoury
   Crate effects" selected, Armoury Crate takes over again.
1. `aura-test.exe list` → record devices/matrix sizes in `aura-sdk-notes.md`. Confirm the
   Azoth shows up as an Aura keyboard.
2. `aura-test.exe cycle` → every selected device changes color and Armoury Crate takes
   over again afterwards.
3. `logiled-harness.exe <G HUB dll>` then `logiled-harness.exe LumaBridge_x64.dll` →
   the mouse behaves identically in both runs, and Aura follows in the second.
4. Process Monitor on BF1 (see `logiled-exports.md`), install, play, read the log.
5. A Chroma game, a GameSense game, and a Corsair game (Games page), one at a time.
   Each should show up on the Lighting page while it runs.

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

## Milestone 5: app (done)

Implemented in `src/app`. Follow-ups:
- Per-device colors in Manual mode (e.g. RAM one color, fans another).
- More manual effects (spectrum cycle, wave across devices).
- Game-specific profiles ("in BF1 use brightest-key mode").

## Later

- **Chroma reader front-end** for signature-checking games (genuine Razer runtime +
  read its effect stream, Artemis-style).
- **Corsair iCUE SDK 4 emulator** (session API, string device ids).
- **Razer Chroma REST API** (`localhost:54235`, used by some web and Unity titles) in the
  app's HTTP server.
