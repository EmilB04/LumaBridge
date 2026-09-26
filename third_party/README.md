# third_party

**Nothing is required here to build.**

- **Logitech LED SDK** — the proxy re-declares the export signatures itself
  (`src/proxy-dll/logiled_api.h`), so `LogitechLEDLib.h` isn't needed. If you download
  the SDK from Logitech, you can drop `LogitechLEDLib.h` here to diff against when a new
  SDK version adds exports.
- **ASUS Aura SDK** — the bridge talks to the Aura COM server through late-bound
  `IDispatch`, so no `AuraServiceLib` type library or headers are needed. Only the runtime
  (Armoury Crate / Aura LightingService) must be installed on the machine.
- **Razer Chroma SDK** — the emulator re-declares the types it reads
  (`src/chroma-emu/chroma_types.h`). `RzChromaSDKTypes.h` can go here as a reference.

These SDKs are not redistributable. Everything in this folder except this README is
git-ignored.
