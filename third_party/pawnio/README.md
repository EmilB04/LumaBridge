# PawnIO (bundled, unmodified)

LumaBridge reaches the SMBus (RAM lighting), the board's monitoring chip (fans, board
temperatures) and the CPU's temperature and power through **PawnIO**, a signed Windows driver that
only runs small signed modules. Windows only loads Microsoft-signed kernel drivers, so
LumaBridge uses PawnIO instead of shipping a driver of its own. All the hardware logic
(which registers, what they mean, what may be written) is LumaBridge's own code; PawnIO
provides the signed access.

These files are redistributed **unmodified**; the release zip puts them in `pawnio\`, and
`scripts\Install-Helper.ps1` installs them (Devices → Hardware access → Set up).

| File | Version | SHA-256 | From |
|---|---|---|---|
| `PawnIO_setup.exe` | 2.2.0.0 | `1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032` | https://github.com/namazso/PawnIO.Setup/releases (latest) |
| `modules/SmbusPIIX4.bin` | 0.2.11 | `91f9b4b1c39e3d399ce48477a89d8f6bd3e58a2241064a4daec3a1513dff56e5` | https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11 (as bundled by LibreHardwareMonitor) |
| `modules/SmbusI801.bin` | 0.2.11 | `a0f7d066e7efda28c0e754c1f52dbd8dc280d388ba8575b514a80cb67490530c` | https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11 (`release_0_2_11.zip`) |
| `modules/LpcIO.bin` | 0.2.11 | `b3896a1cab0d808fca31fe2ebcae045d59dac690da87b17c858bb8da357eb45e` | same |
| `modules/AMDFamily17.bin` | 0.2.11 | `dae74615761b78bdf064dfb3e136252ddcc6fc727d88f14738d0e5800d427a91` | same |
| `modules/IntelMSR.bin` | 0.2.11 | `d6ed85d65ab17a22f813ef98207d6d537155ee2ded5976a21cb48413c9b92e5f` | https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11 (`release_0_2_11.zip`) |

The driver refuses modules that aren't signed by PawnIO's author, so a modified module
would simply not load.

## Licenses and source

- PawnIO (driver and installer): GNU GPL v2 or later, with an exception for independent
  programs that talk to it only through its device I/O control interface (which is how
  LumaBridge uses it). See `COPYING`. Source: https://github.com/namazso/PawnIO. The
  installer states it "can be redistributed unmodified".
- PawnIO modules: GNU LGPL v2.1 or later. See `modules/COPYING`. Source:
  https://github.com/namazso/PawnIO.Modules (`SmbusPIIX4.p`, `SmbusI801.p`, `LpcIO.p`, `AMDFamily17.p`, `IntelMSR.p`).
