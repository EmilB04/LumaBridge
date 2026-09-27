# Game support

What LumaBridge does for specific games. The table lives in code in
[`src/app/games/game_profiles.h`](../src/app/games/game_profiles.h), and each built-in feed
has a tested effect engine next to it.

## Built-in game feeds

These use official data the game publishes on your own PC. Nothing is loaded into the game,
so anti-cheat isn't involved. Set them up on the **Games List** page: click the game's tile.

| Game | Source | Effects |
|---|---|---|
| Counter-Strike 2 | Valve **Game State Integration**. LumaBridge writes `game\csgo\cfg\gamestate_integration_lumabridge.cfg`, and CS2 then POSTs its state to `127.0.0.1:49715`. Restart CS2 after setup. | Team color (CT blue / T gold), freeze time breathing, low health (≤ 25) red pulse, flashbang white-out that fades with the flash, burning flicker, bomb planted: red blink speeding up from 1 to 5 Hz over the 40 s fuse, explosion and defuse bursts, kill (green) and headshot (gold) flashes, round won (rainbow) and lost (dim red), and dim team color while dead. |
| Rocket League | Psyonix **Stats API**. Off by default; LumaBridge sets `PacketSendRate` in `TAGame\Config\DefaultStatsAPI.ini` (with a backup) and reads the JSON events on the configured port (49123 by default). Restart the game after switching it on. | Both team colors around each fan, a strobe then a comet in the scoring team's color on a goal, faster in overtime, and the winner's color breathing at the end. |
| War Thunder | The game's **local status page** at `http://127.0.0.1:8111` (the one its browser map uses). Always on. | Tanks: crew health from green through yellow to red, a red flash when crew is lost, and a pulse when crew is low. Aircraft: blue base, war emergency power flicker, low fuel pulse. |

Tested in `tests/test_core.cpp` with recorded-style payloads. If a game's feed doesn't do
what's described, the app log (`lumabridge-app.log`) shows whether data arrived
(`games: ...` lines).

## Games that light up through a vendor SDK

| Game | Notes |
|---|---|
| Overwatch 2 | Razer Chroma. Install Razer Chroma on the Integrations page. Blizzard's anti-cheat may refuse an unsigned DLL. |
| Battlefield 1 / V / 2042 / 6 | Logitech LIGHTSYNC through G HUB, but EA's anti-cheat keeps LumaBridge's DLLs out, so that lighting only reaches G HUB. While they run, LumaBridge reads the game from the screen image instead: every device shows the screen's colors, flashes red on a hit (red at the screen's edges), breathes red at low health (red edges that stay) and dims while you're dead (a grey screen). Logitech gear too: Battlefield 2042 lights a G502 X Plus black itself; the Logitech devices page can hand it to the game instead. Reading the game's lighting back out of G HUB is being investigated with `tools\ghub-probe.exe --scan`. |

## Screen colors

For games without dynamic lighting (e.g. BOMBANANA!, NO BACKUP, RV There
Yet?), LumaBridge can mirror the screen:
- It watches the primary monitor with the Desktop Duplication API, which sees the finished
  image and never the game.
- It summarizes the left and right halves about 12 times a second, favoring colorful pixels.
- The two colors blend around each fan.

Turn it on for all such games on the Lighting page (Auto), or per game on the Games List page
("Without game lighting": Default / Screen colors / My idle choice).

## Not games

DSX (a DualSense controller utility) is recognized and ignored in Auto mode.
