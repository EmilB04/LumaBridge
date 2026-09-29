# Handoff: where LumaBridge stands and how to work on it

For the rules (branches, commits, releases, what never to build), see
[CLAUDE.md](../CLAUDE.md). This file is the working knowledge: the state of the project,
how to build and check it from a Linux container, and what's open.

## State (v0.22.0, pre-release)

Released through GitHub Actions, one release per round of the owner's requests. The latest
notes are in [docs/releases](releases/). Recent rounds added:

- **My setup** (3D PC and desk, live), the Lighting page preview in 3D or 2D, dashboard
  graphs, the power supply's load, NZXT Kraken readings, AIO / GPU on Devices, monitors
  placed as Windows arranges them, memory slots drawn as the board's slots.
- Closing the window exits the app (Settings can keep it in the tray instead); exiting
  always ends the process (8 s watchdog in `main.cpp`).

Confirmed on the owner's PC: the Kraken readings (liquid, pump, fans, read alongside NZXT
CAM), the 3D view with their three monitors, the Azoth and G502 lighting.
Not yet confirmed there: the dashboard's power total and PSU bar (v0.22.0; needs CPU and
GPU power readings, which Wine lacks).

## Where things are

| Area | Files |
|---|---|
| All the UI (pages, cards, 3D scene builder) | `src/app/ui.cpp` (large; search for `void XxxPage`, `void WXxx(DashCtx&` for dashboard cards, `namespace view3d` for the 3D scene) |
| UI state between frames | `src/app/ui.h` (`UiState`) |
| Settings saved in the INI | `src/app/app_settings.h/.cpp` (`Prefs`, `Load`/`SaveAll`) |
| The controller (devices, games, output, tick) | `src/app/controller.h/.cpp` |
| Small software 3D renderer | `src/app/scene3d.h` |
| Case layout model (fan slots, cooler, desk spots, airflow) | `src/app/pc_layout.h` |
| Monitors: Windows query / desk layout | `src/app/displays.cpp`, `src/app/display_layout.h` |
| USB device catalog (RGB brands, AIOs, PSUs) | `src/app/device_catalog.h` |
| NZXT Kraken status reader (HID, liquidctl protocol) | `src/app/peripherals/nzxt_kraken.h/.cpp` |
| Graph history, PSU draw estimate | `src/app/perf_history.h` |
| Sensors (NVML, PDH, LibreHardwareMonitor, own helper) | `src/app/sensors/` |
| Built-in game lighting (CS2, Rocket League, Flight Simulator, DCS, ...) | `src/app/games/`, `docs/games.md` |
| SDK stand-ins (Logitech, Razer, Corsair, Alienware, SteelSeries) | `src/integrations/` |
| Unit tests (portable; run on Linux) | `tests/test_core.cpp` |
| Release notes | `docs/releases/vX.Y.Z.md` |
| README screenshots | `docs/images/app-mysetup.png`, `app-lighting.png` |

## Building and checking from Linux

The container needs `cmake ninja-build mingw-w64 wine xvfb xdotool imagemagick`; install
them with apt if missing. Use a work directory outside the repo (e.g. the session's
scratchpad) so builds and the Wine prefix aren't committed.

```bash
W=/path/to/work   # e.g. the scratchpad
# Windows app (mingw cross-build, x64)
cmake -S . -B $W/build/x64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-x64.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C $W/build/x64            # watch for errors and warnings
# Portable unit tests, run natively
cmake -S . -B $W/build/native -G Ninja && ninja -C $W/build/native && $W/build/native/test_core
```

CI (`.github/workflows/build.yml`) builds x64 and Win32 with MSVC and runs the tests on
every push, so avoid GNU-only extensions. MinGW needs `extern "C"` around `<hidsdi.h>` /
`<hidpi.h>`.

### Looking at the UI in Wine

```bash
tools/dev/wine-run.sh $W                      # starts the app, 1400x950 window on :99
tools/dev/wine-shot.sh $W dash 90 157         # click Dashboard, save $W/shots/dash.png
```

The first start after a fresh Xvfb can take long enough to time out; run
`wine-run.sh` again (or start it in the background and wait). Then read the PNG to look
at it. Scrolling: `xdotool mousemove X Y; xdotool click 5` (down)
or `click 4` (up). Right-drag: `mousedown 3` / `mouseup 3`. Stop the app with
`WINEPREFIX=$W/wineprefix wineserver -k`.

Wine has no RGB hardware, sensors or USB devices, so lit devices, power readings and the
Kraken don't appear. For screenshots of those, add a **temporary** block at the end of
`view3d::Gather()` in `ui.cpp`, guarded by `if (getenv("LUMA_DEMO"))`, that fills the
`Model` (fans RGB, `m.aio`, `m.kraken`, `m.screens`, keyboard / mouse devices), run with
`LUMA_DEMO=1`, and **remove it before committing**. The README screenshots were made this
way. The lighting only shows when not paused: the run script clears `Running=1` in the INI
so the app doesn't think it crashed.

## How the owner works

- Messages list several things at once, often with screenshots; do all of them, then
  release. They like the work pushed and released without being asked.
- Replies: what changed, what was checked (build, tests, Wine), what couldn't be tested,
  and what to check on their PC.
- When something depends on their hardware (a new USB device, sensors), add diagnostics
  that show on screen and in the log (e.g. the Kraken card shows Windows' error code), so
  their next screenshot tells you what's wrong.

## Open ideas (not promised)

- Read Corsair HXi / RMi power supplies' real output power over USB (liquidctl's
  `corsair_hid_psu` protocol), instead of the estimate.
- More AIOs' status (Corsair iCUE coolers) like the Kraken.
- Per-key output on the Azoth from games' per-key frames (roadmap milestone 4).
