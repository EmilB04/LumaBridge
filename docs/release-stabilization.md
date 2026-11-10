# Release stabilization — 2026-11-10

Release-readiness work included in v0.43.0.

## Changes

- Stop requests reach output workers before joins. Windows synchronous device I/O can be
  cancelled; Aura retains live worker resources until its thread actually ends.
- GameSense owns and reaps its connection threads. Both its listener/client loops and the
  CS2 listener poll with deadlines. OpenRGB connection, reads and writes have deadlines
  and stop checks; a missing server no longer blocks app exit.
- Intentional exit is recorded before cleanup. Normal exits do not start recovery or leave
  a false crash-loop marker. Reopening the app cancels background recovery first.
- Handoff uses delivery tokens and successful-write acknowledgements, including RAM's
  completed SMBus sequence. Bluetooth recovery allows its five-second handshake. A missing
  acknowledgement is logged rather than silently reported as success. Individual native
  handbacks use the final-color path too; a Nollie handoff bypasses boot only on its channel.
- Helper code, app upgrade detection and the elevated installer now agree on task version 8.
  The old wire layout stays compatible; new acknowledgement fields are appended.
- Auto zones can request capture while Everything is Manual. Base idle handback leaves
  active zones running. Explicit Stop lighting and crash pause still stop every zone.
  Per-zone game status is used for mouse/keyboard inactivity behavior.
- Device/zone/hub/core settings reject nonfinite numbers and unsafe integer conversions.
  Unrelated slider edits no longer synchronously rewrite handoff settings.
- The editors share bounded effect metadata. Duration-driven boot reveals hide their
  ignored speed control and use the output planner for the reveal preview.
- Memory diagnostics show measured bus transaction time and successful frame count.
- Terminate/abort captures exception details, a stack and a minidump. The exact cause of
  the older terminate-only crash remains unconfirmed; it is not claimed as reproduced.

## Validation

- Portable core tests passed, including invalid numeric inputs, metadata bounds, boot
  timing/one-shot behavior, zone capture demand, ownership and delivery receipts.
- The original NaN decoder/render reproducer now rejects the input without a sanitizer error.
- Windows integration tests passed on native Windows and Wine: temporary INI round trips,
  zone deletion persistence, GameSense idle clients and repeated listener shutdown/start,
  game-feed worker shutdown and cancellation of a blocked synchronous pipe read.
  The CS2 port was occupied during the native run; its listener/restart checks also ran
  with the port free in the isolated Wine run.
- Helper version consistency and existing handback failure-path PowerShell tests passed.
- Full-app checks in an isolated Wine prefix passed: normal exit 1.29 seconds, repeat exit
  1.39 seconds, reopen during recovery 0.88 seconds, final exit 0.83 seconds. Forced exit
  started recovery; normal exits added no watchdog warnings or false crash markers.
- Native startup/exit on the owner's PC completed with final-frame acknowledgements and
  exit code 0 in 1.18 seconds. Helper version 8 was installed and its executable hash checked.

## Checks that still require observation

Successful writes cannot prove how the physical lights look. Before publication, visually
check each RAM stick and its order, Bluetooth final-color retention, unplug/reconnect and
sleep/wake. Other lighting software or firmware can overwrite an acknowledged final frame.
Aura's motherboard and ARGB headers share native ownership with Armoury Crate, so native
handback remains paired. These hardware checks are not represented as automated passes.

## Reproduce

- `cmake --build <native-build> && ctest --test-dir <native-build> --output-on-failure`
- Windows: build and run `test_windows`; CTest also runs the helper-version and handback
  PowerShell checks when PowerShell is available. CI already invokes CTest on both Windows
  architectures.
- With an isolated Xvfb display: `python3 tools/dev/check-lifecycle.py --app <app.exe>
  --work /tmp/luma-qa --display :97`. The script uses its own Wine prefix and restores its
  test INI afterward; it never edits the native Windows profile.
