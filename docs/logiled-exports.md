# Logitech LED SDK (LogiLed) exports

Source of truth in code: [`src/integrations/logitech/logiled_api.h`](../src/integrations/logitech/logiled_api.h),
a single X-macro list that generates the typedefs, the `GetProcAddress` table and the
pass-through exports. [`exports.def`](../src/integrations/logitech/exports.def) pins the undecorated
export names.

Signatures follow `LogitechLEDLib.h` from LED SDK 8.87 / 9.x. The header declares no calling
convention, so they are the compiler default, `__cdecl`. On x64 there is only one convention.
Enum parameters (`LogiLed::KeyName`, `LogiLed::DeviceType`) are `int` in the ABI.

## Export table and what the proxy does

| Export | Proxy behavior |
|---|---|
| `LogiLedInit`, `LogiLedInitWithName(name)` | forward; start Aura mirror; returns true if real **or** Aura is available |
| `LogiLedShutdown` | stop mirror (releases Aura to Armoury Crate); forward |
| `LogiLedSetLighting(r,g,b)` | forward; mirror as static ambient color (0–100 % → 0–255) |
| `LogiLedSetLightingFromBitmap(bmp[504])` | forward; 21×6 BGRA grid reduced to one color (`[Mirror] BitmapMode`) |
| `LogiLedSetLightingForTargetZone(dev, zone, r,g,b)` | forward; mirror as ambient |
| `LogiLedFlashLighting(r,g,b,dur,interval)` | forward; flash re-created on Aura (on/off every `interval`, `dur`=0 → forever) |
| `LogiLedPulseLighting(r,g,b,dur,interval)` | forward; cosine fade with period `interval` |
| `LogiLedStopEffects` | forward; Aura back to last static color |
| `LogiLedSaveCurrentLighting` / `LogiLedRestoreLighting` | forward; save/restore the ambient color |
| `LogiLedGetSdkVersion`, `LogiLedGetConfigOption*` (8), `LogiLedSetConfigOptionLabel`, `LogiLedSetTargetDevice` | pure pass-through (false if no real DLL) |
| `LogiLedSetLightingForKeyWith{ScanCode,HidCode,QuartzCode,KeyName}`, `LogiLed{Save,Restore}LightingForKey`, `LogiLedExcludeKeysFromBitmap`, `LogiLed{Flash,Pulse}SingleKey`, `LogiLedStopEffectsOnKey` | pass-through for now. Per-key → Azoth is milestone 4 |

Constants used: `LOGI_LED_BITMAP_SIZE = 21*6*4 = 504` (BGRA), `LOGI_LED_DURATION_INFINITE = 0`,
`LOGI_DEVICETYPE_ALL = 7`.

## How games find the DLL

There are two ways a game ends up loading the LED SDK, and the install mode has to match:

1. **Static `LogitechLEDLib.lib` (most games, likely BF1).** The lib looks up
   `HKCR\CLSID\{a6519e67-7632-4375-afdf-caa889744403}\ServerBinary` and `LoadLibrary`s that
   full path, which with G HUB is `C:\Program Files\LGHUB\sdk_legacy_led_x64.dll`. Because
   the path is absolute, **copying a DLL into the game folder does nothing.** Redirect the
   registry value instead. This is the same technique Aurora and Artemis use for their
   Logitech wrappers.
   - `Install-LogiLedProxy.ps1` (default `-Scope User`) writes an HKCU override. HKCR is the
     merged HKCU+HKLM view with HKCU winning, so if the lib opens HKCR this is enough, needs
     no admin, and doesn't touch anything Logitech owns.
   - If the log shows the proxy never loading, the lib is reading HKLM directly: use
     `-Scope Machine`, which rewrites HKLM and saves the original path to `RealDllPath`.
     Note that a G HUB update may put the original value back.
2. **Game ships its own DLL** (e.g. `LogitechLedEnginesWrapper.dll` or `LogitechLed.dll`
   next to the exe). Use `-GameDir <folder> -DllName <name>`, a classic search-order hijack.
   The game's original copy is backed up.

The proxy always finds the real DLL through `RealDllPath` first, then **HKLM** (never HKCR,
so an HKCU redirect can't make it load itself), then the default G HUB/LGS paths. It also
refuses any candidate that resolves to its own file.

### Confirming what BF1 does (do this first)

1. Run Sysinternals **Process Monitor** with filters *Process Name is bf1.exe* and *Path
   contains `led`* (plus a second filter on *Path contains `a6519e67`*).
2. Start BF1 and reach the main menu.
3. `RegQueryValue … ServerBinary` followed by `CreateFile …\LGHUB\sdk_legacy_led_x64.dll`
   means mode 1. A `CreateFile` for a DLL in the game folder means mode 2. Also note whether
   the registry read goes to `HKCR`/`HKCU\Software\Classes` or straight to `HKLM`.

The `CLSID` and file names above come from G HUB installs and community wrappers, not
from official Logitech documentation. Check them in `regedit` on your machine.

## Behavior notes

- Nothing runs in `DllMain`. Config, log and real-DLL loading happen on the first exported
  call; the Aura worker starts in `LogiLedInit`.
- Aura writes run on a dedicated worker thread (its own single-threaded COM apartment), coalesced to ≤ `MaxUpdateHz`.
  The game thread only takes a mutex and stores the new color.
- If G HUB isn't running or installed, the proxy still returns success and lights Aura
  through the app on its own.
- Without the LumaBridge app the proxy only passes calls through to G HUB. It never loads
  the Aura SDK inside the game unless `[Aura] DirectFromGames=1` is set.
