<p align="center"><img src="../android/images/logo.svg" alt="Polar logo" width="128"></p>

# Polar

Polar is a fork of [Banana Native](https://github.com/moceure/BananaNative), based on [nxnative](https://github.com/seedds/nxnative).

It runs Switch and Switch 2 games on Android phones from your own console dumps. Like Wine, it does not emulate hardware: the game's ARM64 code runs natively on the phone's CPU while Polar handles Horizon OS system services and translates NVN graphics to Vulkan (with ahead-of-time shader translation to SPIR-V).

> [!NOTE]
> Unofficial and experimental. This repository contains no Nintendo code, firmware, or keys, and does not decrypt anything. Use only dumps of games you own.

## Requirements

- Android 13+ phone with a Snapdragon (Adreno) GPU; Android 12/12L works with the experimental Android 12 build
- Your own game dump (`base/`, `updates/<version>/`, `dlc/`, or a `.nup` package)

## Setup

1. Install the Android APK (package name `dev.polar.android`) and grant **All files access**.
2. Prepare a game pack on PC/Mac (`scripts/polar.bat` or `scripts/polar.command`) or directly on the phone via **Add game** → **Prepare**.
3. Launch the game from the app.

## Games

The [games table](../dm/games/README.md#games) lists every title the app knows and links to its note. Each note says what was tested and what is still open; check your game's note for known problems before filming a video.

| Game | Version | Title ID |
| --- | --- | --- |
| [The Legend of Zelda: Tears of the Kingdom](../dm/games/0100F2C0115B6000/README.md) | 1.4.3 | `0100F2C0115B6000` |
| [The Legend of Zelda: Breath of the Wild](../dm/games/01007EF00011E000/README.md) | 1.9.0 | `01007EF00011E000` |
| [Pokémon Legends: Z-A](../dm/games/0100F43008C44000/README.md) | 2.0.2 | `0100F43008C44000` |
| [FINAL FANTASY RESONANCE](../dm/games/0100606026F12000/README.md) | 1.0.1 | `0100606026F12000` |
| [South Park: The Fractured but Whole](../dm/games/01008F2005154000/README.md) | 1.0.5 | `01008F2005154000` |
| [Hogwarts Legacy](../dm/games/0100F7E00C70E000/README.md) | 1.0.5 | `0100F7E00C70E000` |
| [DRAGON BALL: Sparking! ZERO](../dm/games/010035F022078000/README.md) | 2.0.76 | `010035F022078000` |
| [Luigi's Mansion 3](../dm/games/0100DCA0064A6000/README.md) | 1.4.0 | `0100DCA0064A6000` |
| [Pokémon Scarlet](../dm/games/0100A3D008C5C000/README.md) | 1.0.0 | `0100A3D008C5C000` |
| [What Remains of Edith Finch](../dm/games/010038900DFE0000/README.md) | 1.0.0 (base) | `010038900DFE0000` |
| [Sid Meier's Civilization VII](../dm/games/0100C3601518C000/README.md) | 1.5.1 | `0100C3601518C000` |
| [LEGO Star Wars: The Skywalker Saga](../dm/games/010042D00D900000/README.md) | 1.10.0 | `010042D00D900000` |
| [Hellblade: Senua's Sacrifice](../dm/games/010044500CF8E000/README.md) | 1.1.0 (base) | `010044500CF8E000` |
| [No Man's Sky](../dm/games/0100853015E86000/README.md) | v5308416 (7.10.0) | `0100853015E86000` |
| [Super Mario Odyssey](../dm/games/0100000000010000/README.md) | 1.4.1 | `0100000000010000` |

## Licence and credits

- Android build & internals: [android/README.md](../android/README.md)
- Player guides: [docs/guides/getting-started.md](../docs/guides/getting-started.md) · [docs/guides/troubleshooting.md](../docs/guides/troubleshooting.md)
- Built on [Banana Native](https://github.com/moceure/BananaNative) and [nxnative](https://github.com/seedds/nxnative) by seedds (MIT). The Android app is **GPL-3.0-or-later** as a whole ([android/NOTICE.md](../android/NOTICE.md)).
