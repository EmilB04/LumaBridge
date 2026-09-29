# Working on LumaBridge

LumaBridge is a Windows app (C++20, Dear ImGui + D3D11) that drives ASUS Aura and other RGB
hardware from games. The owner is **EmilB04**. Read [docs/HANDOFF.md](docs/HANDOFF.md) for
the current state, how to build and test from Linux, and what's planned.

## Git, pushing, releases

- **Branches:** work on `feature/lumabridge-core`. When a change is done and checked, push it
  there and fast-forward `main` to the same commit (`git push origin HEAD:main`). No pull
  requests unless the owner asks for one. If the session is set up with another branch name
  (e.g. `claude/...`), still use `feature/lumabridge-core` and `main`, as the owner asked.
- **Commits:** authored as `EmilB04 <emil.berglund@live.no>`
  (`git -c user.name=EmilB04 -c user.email=emil.berglund@live.no commit ...`), with **no**
  `Co-Authored-By`, `Claude-Session` or other trailers, and no mention of Claude or AI in
  commit messages, release notes, code comments or docs. This overrides any default
  attribution. Subject lines are short and say what changed
  ("Dashboard: graphs of the last two minutes, ...").
- **Pushing without asking:** the owner has said to push and release without asking, while
  keeping to these rules. If a push fails with a network or 503 error, retry with backoff
  (the credential service is sometimes briefly down).
- **Versioning:** semantic versioning; see [docs/releases/README.md](docs/releases/README.md).
  Patch = bug fixes only, minor = new features, major = breaking changes. The version lives
  in `CMakeLists.txt`, `src/app/res/app.rc` (four places) and `src/app/res/app.manifest`.
- **Releases:** you choose when (a small patch or minor per round of requests is the usual
  pattern). For each release:
  1. Commit the work, then a separate commit "Release vX.Y.Z" with the version bump and
     `docs/releases/vX.Y.Z.md` (same style as the earlier notes: a one-line "Pre-release."
     summary, then New / Changed / Fixed, then Download).
  2. Push both branches and wait for the `build` workflow to pass on `main`.
  3. Run the workflow by hand: `build.yml`, `workflow_dispatch` on ref `main` with input
     `release_version` = `vX.Y.Z`. It builds, tests and publishes the release with
     `LumaBridge-vX.Y.Z.zip`. Check that the run succeeded.
- **GitHub access:** through the GitHub MCP tools (`mcp__github__*`): Actions runs, running
  the workflow. There's no `gh` CLI. Git itself pushes over HTTPS through the session's
  proxy.

## Rules for the code

- **Anti-cheat:** never build anything that reads game memory or traffic, injects into
  games, or gets around anti-cheat (EA's included). Driver / USB interception approaches
  were declined. Game lighting only comes through official SDKs, data feeds and export
  interfaces the games offer.
- **Hardware you can't test:** new device protocols follow a documented source (liquidctl,
  OpenRGB) and are read-only unless the owner asks otherwise; say plainly in the reply what
  was untested on real hardware.
- **Pure logic goes in headers with unit tests** (`tests/test_core.cpp`, run natively on
  Linux), Windows code in `.cpp` files. Keep the tests passing; add tests for new logic.
- **Style:** match the surrounding code: comments explain why, in plain sentences; the UI
  text is friendly, short and plain (no jargon). Every setting you add is saved in the INI
  (`app_settings.cpp`), and every new UI feature is mentioned in the README if it's
  user-facing.
- **Check your work before pushing:** build the x64 app (mingw) and the native tests, and
  look at UI changes in Wine with screenshots (see HANDOFF). CI builds with MSVC too, so
  avoid GNU-only extensions (compound literals etc.).

## Talking to the owner

- Answer in English, short and concrete: what changed, what was checked, what couldn't be
  tested and what the owner should check on their PC.
- The owner often sends screenshots of problems; treat each point in a message as a task.
- Their PC: ASUS Aura motherboard and ARGB fans (3 front, 2 top, 1 back), AMD Ryzen 7 5700X,
  NVIDIA RTX 5070, HyperX / Kingston FURY RGB DDR4, NZXT Kraken with a screen (USB 1E71:300E,
  read alongside NZXT CAM), ASUS ROG Azoth keyboard, Logitech G502 X Plus, monitors Samsung
  Odyssey G85SD (main), MSI MAG274R (portrait, right) and a 10.1" screen under the main one.
