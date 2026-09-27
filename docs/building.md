# Building

## Windows (release builds)

Requirements: Visual Studio 2022 or newer with *Desktop development with C++* (includes CMake and
git). No SDK downloads are needed (see [third_party/README.md](../third_party/README.md)).
The first configure downloads Dear ImGui v1.91.9 (MIT) from GitHub through CMake
FetchContent. Offline, clone it yourself and pass
`-DFETCHCONTENT_SOURCE_DIR_IMGUI=<path>`.

**IDE:** *File → Open → Folder…* on the repo, pick configure preset **vs-x64**, then
*Build → Build All*.

**Command line** (Developer PowerShell):

```powershell
cmake --preset vs-x64
cmake --build --preset vs-x64
ctest --test-dir build/x64 -C Release
cmake --install build/x64 --config Release --prefix dist   # app, DLLs, tools, scripts, ini example
```

Use preset `vs-x86` for 32-bit games (DLLs only; the app is x64). BF1 is 64-bit only.
For 32-bit Chroma/LightFX games, copy the x86 DLLs into `integrations\x86\`, and the install
script will put them in SysWOW64 too. The release zip already has them there.

Build output lands in `build/x64/Release/`. `cmake --install` arranges it the way the
release zip is laid out: the app at the top, game DLLs in `integrations\`, test tools in
`tools\`, install scripts in `scripts\`.

Files:

| File | Purpose |
|---|---|
| `LumaBridge.exe` | the app (x64 builds only) |
| `LumaBridge_x64.dll` | LogiLed proxy |
| `RzChromaSDK64.dll` | Razer Chroma emulator |
| `CUESDK.x64_2019.dll` | Corsair iCUE emulator |
| `LightFX.dll` | Alienware LightFX emulator |
| `aura-test.exe` | milestone 2: list Aura devices / set a color |
| `logiled-harness.exe` | drives any LogiLed DLL like a game would |
| `test_core.exe` | unit tests |

The CRT is linked statically, so the DLLs don't depend on a VC++ redistributable inside
the game process.

## Linux / WSL (compile check only)

You can't run anything here, but you can catch compile and link errors and check the export
tables:

```bash
sudo apt install mingw-w64 cmake
cmake --preset mingw-x64 && cmake --build --preset mingw-x64
x86_64-w64-mingw32-objdump -p build/mingw-x64/LumaBridge_x64.dll | grep -A40 'Ordinal/Name'

# portable unit tests, run natively
cmake -S . -B build/native && cmake --build build/native && ctest --test-dir build/native
```

## CI

`.github/workflows/build.yml` builds x64 and Win32 with MSVC on every push, runs the unit
tests, and uploads a ready-to-copy `LumaBridge-<arch>` artifact (app, DLLs, tools,
scripts, ini example).

## UI screenshots without Windows

The images in `docs/images` were rendered offscreen on Linux from the real `ui.cpp`, with
the Windows-only classes stubbed out and a small CPU rasterizer for ImGui's draw lists.
They show the layout; on Windows the font is Segoe UI.
