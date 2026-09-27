# third_party

**Nothing is required here to build.**

- **Logitech LED SDK** — the proxy re-declares the export signatures itself
  (`src/integrations/logitech/logiled_api.h`), so `LogitechLEDLib.h` isn't needed. If you download
  the SDK from Logitech, you can drop `LogitechLEDLib.h` here to diff against when a new
  SDK version adds exports.
- **ASUS Aura SDK** — the bridge talks to the Aura COM server through late-bound
  `IDispatch`, so no `AuraServiceLib` type library or headers are needed. Only the runtime
  (Armoury Crate / Aura LightingService) must be installed on the machine.
- **Razer Chroma SDK** — the emulator re-declares the types it reads
  (`src/integrations/razer/chroma_types.h`). `RzChromaSDKTypes.h` can go here as a reference.

These SDKs are not redistributable. Everything in this folder except this README and
`pawnio/` is git-ignored.

- **PawnIO** (`pawnio/`) — the signed driver installer and modules LumaBridge ships in its
  release zip for RAM lighting and the built-in sensors. These may be redistributed
  unmodified, so they are checked in; versions, checksums, licenses and sources are in
  [`pawnio/README.md`](pawnio/README.md).
