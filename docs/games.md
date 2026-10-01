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
| F1 24, F1 25 | The games' own **UDP telemetry**. Switch it on in the game: Settings > Telemetry Settings > UDP Telemetry On, IP `127.0.0.1`, port 20777 (or the port on its page in LumaBridge). Reads the Car Telemetry packet only. | Rev lights from the game's own shift-light reading: blue at low revs, then green, yellow and red as they climb, and a fast red flash once they're full. Only with the engine running (no explicit "on track" flag in this packet). **Not checked against a live packet capture** (no PC to run the game on) - the byte layout follows EA's public UDP specification, which has been stable across F1 22-25. |
| BeamNG.drive | The game's own **OutGauge** output (UDP). Switch it on in the game: Options > Other > Protocols (Live for Speed OutGauge) on, IP `127.0.0.1`, port 4444 (or the port on its page in LumaBridge). OutGauge has no redline, so LumaBridge uses the highest engine speed seen this session. | Rev lights (blue, green, yellow, red, a red flash at the limit or on the game's shift light) and an amber pulse for the oil and battery warnings. Only with the engine running. **Not checked against the running game.** |
| DiRT Rally, DiRT Rally 2.0 | The games' own **UDP telemetry**. Switched on in `Documents\My Games\DiRT Rally 2.0\hardwaresettings\hardware_settings_config.xml` (DiRT Rally: the `DiRT Rally` folder): `<udp enabled="true" extradata="3" ip="127.0.0.1" port="20777" delay="1" />`. Use another port than F1 if both are on. | Rev lights from the engine speed, blue through red. Only on a stage (the menus send zeros). **Not checked against the running game**: the byte offsets (gear 132, engine rate 148, max rpm 252, idle rpm 256) follow the layout the community documents. |
| Automobilista 2, Project CARS 2 | The games' own **UDP telemetry** (the "Project CARS 2" format). Options > System > Shared Memory: Project CARS 2 UDP, a frame rate of 1 or more; it sends to port 5606. Reads only the telemetry packet. | Rev lights from the engine speed, a red flash at the limiter, an amber pulse for an engine warning. Only with the engine running. **Not checked against the running games.** |
| X-Plane 11 / 12 | The sim's own **UDP data output**. Settings > Data Output: tick the Network via UDP box for rows 3 (speeds) and 4 (Mach, VVI, G-load), and send to IP `127.0.0.1`, port 49003. | Blue in flight, amber then red as the G-load climbs (red strobe from 4 G), magenta under negative G, a slow dim blue while parked. Row numbers are the ones X-Plane's Data Output screen shows and can differ between versions. **Not checked against the running sim.** |
| Elite Dangerous | The game's own **Status.json** in `Saved Games\Frontier Developments\Elite Dangerous`. Always written; nothing to set up. | The cockpit's orange in normal space, cyan in supercruise, red with hardpoints out, a slow pulse while the FSD charges and a white flash on the jump, an amber pulse on low fuel, a red pulse in danger, a red-orange strobe when interdicted, a fast red strobe when overheating, and dim blue while docked or landed. |
| Microsoft Flight Simulator (2020 / 2024) | **SimConnect**, the sim's own add-on interface. LumaBridge loads `SimConnect.dll` from Microsoft's free Flight Simulator SDK (found through the SDK's `MSFS_SDK` / `MSFS2024_SDK` setting or its default folder), or from next to `LumaBridge.exe`, and connects while the sim runs. Install the SDK from the sim: Options > General Options > Developers > Developer Mode, then Help > SDK Installer in the Developer menu. | The sky's color by the sim's time of day (blue, orange at dawn and dusk, deep blue at night), dimmed while parked with the engines off; the aircraft's strobe lights as white flashes and its beacon as red flashes over it, low fuel (under 10 %, in the air) amber breathing, overspeed warning amber strobe, stall warning red strobe, and a slow red after a crash. Only during a flight (not in the menus). |
| DCS World | DCS's **export script** (`Export.lua` in `Saved Games\DCS\Scripts`, the place SRS, Tacview and Helios use). LumaBridge adds `LumaBridge.lua` and one line to `Export.lua` that loads it (other scripts there keep working; Remove takes both out again). It reads the aircraft through DCS's export functions and sends it as JSON over UDP to `127.0.0.1:49717`, 20 times a second. Servers that forbid exporting your own aircraft leave it quiet. | The sky's color by the mission's time of day, dimmed on the ground with the engines off; three greens with the gear down in the air; red from 6 G, deeper towards 9 G, and magenta under negative G; the master warning as a red strobe. |

Tested in `tests/test_core.cpp` with recorded-style payloads. If a game's feed doesn't do
what's described, the app log (`lumabridge-app.log`) shows whether data arrived
(`games: ...` lines).

## Games that light up through a vendor SDK

| Game | Notes |
|---|---|
| Overwatch 2 | Razer Chroma. Install Razer Chroma on the Integrations page. Blizzard's anti-cheat may refuse an unsigned DLL. |
| Battlefield 1 / V / 2042 / 6 | Logitech LIGHTSYNC through G HUB, but EA's anti-cheat keeps LumaBridge's DLLs out, so that lighting only reaches G HUB. While they run, LumaBridge hands your Logitech gear to the game, so it shows the game's own effects through G HUB. Your other devices show your choice for "When no game is running", or the screen's colors if you pick Screen colors on the game's page. |
| Euro Truck Simulator 2, The Last of Us Part I, The Sims 4 | Logitech LIGHTSYNC. With Logitech LIGHTSYNC set up on the Integrations page LumaBridge catches it and shows it on every device; otherwise LumaBridge hands your Logitech gear to the game while it runs. |
| Apex Legends, PUBG: BATTLEGROUNDS, HITMAN World of Assassination | Razer Chroma. Install Razer Chroma on the Integrations page (and switch Chroma on in the game's options if it has one). Their anti-cheat may refuse LumaBridge's DLL; then use Screen colors. |
| Marvel Rivals | Razer Chroma, switched on in its launcher. It talks to Razer's own Chroma app, so it lights Razer gear through Razer Synapse only. |
| Civilization VI, Total War: WARHAMMER III, Terraria, Assetto Corsa, iRacing, Grand Theft Auto V, Factorio, American Truck Simulator | Logitech LIGHTSYNC. With Logitech LIGHTSYNC set up on the Integrations page LumaBridge catches it and shows it on every device; otherwise LumaBridge hands your Logitech gear to the game while it runs. Which events drive which colors isn't publicly documented for these. |
| Destiny 2, Rainbow Six Siege, Red Dead Redemption 2, Call of Duty | No lighting of their own (only fixed, fan-made profiles). Use Screen colors. |

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

DSX (a DualSense controller utility) and Wallpaper Engine are recognized and ignored in Auto mode.

**Every device in step.** An effect carries its start time (`fx::Params::epoch`), shared by
the fans, board, memory, keyboard, mouse and any other device, so they all show the same
moment of it; before, each device counted from when it got the effect, and a game changing
the speed (the bomb's fuse) restarted each one separately. During a game's lighting the
Logitech mouse shows it LED by LED (its own effects run on the mouse's clock) and gets a new
frame every 20 ms, with one round trip per frame.
