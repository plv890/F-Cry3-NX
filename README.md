# Far Cry 3 for Nintendo Switch (unofficial port)

Version 1.0

An unofficial, non-commercial port of the Xbox 360 version of Far Cry 3 to the Nintendo Switch, made by static
recompilation with [ReXGlue](https://github.com/rexglue/rexglue-sdk) and drawn with a deko3d renderer. It is not an emulator.
Not affiliated with or endorsed by Ubisoft or Nintendo. Far Cry is a trademark of Ubisoft.

**This repository has no game files.** You need your own copy of the game and its title update. There are no prebuilt
binaries here: the code generated from the game, the shaders made from it and the NRO are not part of this repository.

## Screenshots

Captured on a Switch on builds from 12 to 21 November 2026, older than 1.0: the picture may differ. The overlay is Status Monitor.

![Beach](media/02-beach-ocean.jpg)

| | |
|---|---|
| ![Radio tower](media/01-radio-tower.jpg) | ![Jungle path](media/06-jungle-path.jpg) |
| ![Village cutscene](media/07-cutscene-village.jpg) | ![Cutscene](media/08-cutscene-dennis.jpg) |
| ![Shop](media/05-shop.jpg) | ![Candle scene](media/04-candle-tattoo.jpg) |

## Supported game

Only one edition is supported. The game and the title update must have the same Media ID.

| | |
|---|---|
| Title ID | `5553088C` |
| Media ID | `003B8D49` |
| `default.xex` version | 0.0.0.4 |
| Title update | Title Update 5 for Media ID `003B8D49` (required) |

Title Update 5 for Media ID `6F6EAD36` belongs to another edition and does not fit. Other editions: not verified.

See [INSTALL.md](INSTALL.md) for hashes and steps.

## State of 1.0

- Boots from the HOME menu, loads saves, open world and cutscenes run.
- Target 30 FPS. Usually around 25, with drops below 20. Limited by the CPU.
- Known bugs: lighting and texture glitches in some scenes.
- Music can be turned off by the game's own setting, which is only in the main menu options.
- Tested on **one console only** (firmware 21.1.0, Atmosphere), overclocked: CPU 2091 MHz, GPU 1075 MHz,
  RAM 3000 MHz in the Horizon OC profile. CPU 2.3 GHz: not verified. Overclocking is at your own risk.
- Stock clocks, other console models and firmwares: not verified.
- Installation on a clean SD card: not verified.

## Contents

| Folder | What it is |
|---|---|
| `app/` | the port: hooks, configuration, renderer glue |
| `sdk/` | modified ReXGlue SDK with the Switch layer and the deko3d renderer |
| `shaders/` | shader translation tools |
| `patches/`, `mesa/` | changes to third-party code |
| `tools/` | build tools and `prepare_update.py` |
| `fc3.toml.example` | example settings |

## License and credits

GPL-3.0, see [LICENSE](LICENSE). Parts are under other licenses, see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Based on [fc3-nx](https://github.com/barbaruiva25/fc3-nx) and the projects listed there.
