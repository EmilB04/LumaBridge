# Game support

What LumaBridge does for specific games. The table lives in code in
[`src/app/games/game_profiles.h`](../src/app/games/game_profiles.h), and each built-in feed
has a tested effect engine next to it.

## Built-in game feeds

These use official data the game publishes on your own PC. Nothing is loaded into the game,
so anti-cheat isn't involved. Set them up on the **Games List** page: click the game's tile.

| Game | Source | Effects |
|---|---|---|
| Counter-Strike 2 | Valve **Game State Integration**. LumaBridge writes `game\csgo\cfg\gamestate_integration_lumabridge.cfg`, and CS2 then POSTs its state to `127.0.0.1:49715`. Restart CS2 after setup. | Team color (CT blue / T gold), freeze time breathing, low health (≤ 25) red pulse, flashbang white-out that fades with the flash, burning flicker, bomb planted: a red beat counted from the plant, speeding up from 1 to 5 a second over the 40 s fuse, on every device at the same moment, explosion and defuse bursts, kill (green) and headshot (gold) flashes, round won (rainbow) and lost (dim red), and dim team color while dead. |
| Rocket League | Psyonix **Stats API**. Off by default; LumaBridge sets `PacketSendRate` in `TAGame\Config\DefaultStatsAPI.ini` (with a backup) and reads the JSON events on the configured port (49123 by default). Restart the game after switching it on. | Your team's color around each fan (the team of the car the camera follows; both team colors while spectating), a strobe then a comet in the scoring team's color on a goal, faster in overtime, and the winner's color breathing at the end. |
| War Thunder | The game's **local status page** at `http://127.0.0.1:8111` (the one its browser map uses). Always on. | Tanks: crew health from green through yellow to red, a red flash when crew is lost, and a pulse when crew is low. Aircraft: blue base, war emergency power flicker, low fuel pulse. |
| Dota 2 | Valve **Game State Integration**, like CS2 (same port; Dota 2's posts carry its app id, 570). LumaBridge writes `game\dota\cfg\gamestate_integration\gamestate_integration_lumabridge.cfg`; Dota 2 also needs `-gamestateintegration` in its launch options (Steam > Properties > General). Restart Dota 2 after setup. | Your team's color (Radiant green / Dire red), dimmer at night, breathing while picking heroes, low health (≤ 25 %) red pulse, a white pulse while stunned or hexed, dim while dead, a gold flash on a kill, and a rainbow on a win (dim red on a loss). |
| League of Legends | Riot's **Live Client Data API** at `https://127.0.0.1:2999/liveclientdata/allgamedata`, answered by the game on this PC during every match. Nothing to set up. | Your side's color (blue / red), low health (≤ 25 %) red pulse, dim while dead, a gold flash on your kill, a rainbow on your multikill, a pulse in the dragon's color when your team takes a dragon (purple for baron and herald), and a rainbow on victory (dim red on defeat). |
| Forza Horizon 4 / 5, Forza Motorsport | The games' own **Data Out** telemetry (UDP). Switch it on in the game: Settings > HUD and Gameplay (Forza Motorsport: Gameplay & HUD) > Data Out On, IP `127.0.0.1`, port 5300 (or the port on its page in LumaBridge). | Rev lights from the engine speed: blue at low revs, then green, yellow and red as it climbs, and a fast red flash at the limiter. Only while driving (not in the menus). |

Tested in `tests/test_core.cpp` with recorded-style payloads. If a game's feed doesn't do
what's described, the app log (`lumabridge-app.log`) shows whether data arrived
(`games: ...` lines).

## Games that light up through a vendor SDK

| Game | Notes |
|---|---|
| Overwatch 2 | Razer Chroma. Install Razer Chroma on the Integrations page. Blizzard's anti-cheat may refuse an unsigned DLL. |
| Battlefield 1 / V / 2042 / 6 | Logitech LIGHTSYNC through G HUB, but EA's anti-cheat keeps LumaBridge's DLLs out, so that lighting only reaches G HUB. While they run, LumaBridge hands your Logitech gear to the game, so it shows the game's own effects through G HUB. Your other devices show your choice for "When no game is running", or the screen's colors if you pick Screen colors on the game's page. |

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

**Every device in step.** An effect carries its start time (`fx::Params::epoch`), shared by
the fans, board, memory, keyboard, mouse and any other device, so they all show the same
moment of it; before, each device counted from when it got the effect, and a game changing
the speed (the bomb's fuse) restarted each one separately. During a game's lighting the
Logitech mouse shows it LED by LED (its own effects run on the mouse's clock) and gets a new
frame every 20 ms, with one round trip per frame.
