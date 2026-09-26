# Building

## Windows (release builds)

Requirements: Visual Studio 2022 with *Desktop development with C++* (includes CMake).
No SDK downloads are needed. See [third_party/README.md](../third_party/README.md).

**IDE:** *File → Open → Folder…* on the repo, pick configure preset **vs-x64**, then
*Build → Build All*.

**Command line** (Developer PowerShell):

```powershell
cmake --preset vs-x64
cmake --build --preset vs-x64
ctest --test-dir build/x64 -C Release
cmake --install build/x64 --config Release --prefix dist   # DLLs + tools + ini example
```

Use preset `vs-x86` for 32-bit games. BF1 is 64-bit only.

Output (in `build/x64/Release/`):

| File | Purpose |
|---|---|
| `LumaBridge_x64.dll` | LogiLed proxy |
| `RzChromaSDK64.dll` | Razer Chroma emulator |
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
tests, and uploads a ready-to-copy `LumaBridge-<arch>` artifact (DLLs, tools, scripts,
ini example).
