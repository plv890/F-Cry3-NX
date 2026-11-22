# Installation

Tested on one console only. A clean install has not been verified.

## Only one version of the game works

This port is built for one `default.xex`. The game and the title update must both have Media ID `003B8D49`.

| | |
|---|---|
| Game | Far Cry 3, Xbox 360, disc "Europe, Asia (En,Pl,Ru)" |
| Title ID | `5553088C` |
| Media ID | `003B8D49` |
| `default.xex` version | 0.0.0.4 |
| `default.xex` SHA-256 | `f9c08098c7e435f89899955045608f727c0d1bea13e4de84e06b93cd729539df` |
| Title update | Title Update 5, file `tu00000004_00000000`, 7,712,768 bytes |
| Title update SHA-256 | `367a9caae9b3957aec1bb7258b62ecaaf0efbf3230fd4749bbee2ba156d80c09` |

The game and the title update are not included. Use your own copies.

## Steps

1. Download the archive from Releases and copy its `switch` folder to the root of the SD card. About 6 GB is needed in
   total. `switch/fc3-nx/` then holds `fc3.nro`, `fc3dk_shaders.dslib`, `fc3.toml`, `game_root/` and `update/`.
2. Extract your disc image and copy `default.xex`, all `.dat` and `.fat` files and `worlds/` into `game_root/`.
   `default.xex` must be directly inside `game_root/`. `$SystemUpdate` is not needed.
3. On a PC with Python 3, unpack your title update:

   `python pc/prepare_update.py <default.xex> <tu00000004_00000000> <output folder>`

   Copy `patch.dat` and `patch.fat` from the output folder into `update/`. The update's `default.xexp` is not used.
   Optional: add `--icons` to merge Switch-style button icons, made from your own `common.fat` and `common.dat`.
   Your original files are not changed.
4. Start it in application mode: a HOME forwarder with 39-bit address space, or hbmenu. The Album does not have
   enough memory.
5. No music? Turn it on in the options of the main menu. The option is not available once a game is loaded.

Keep `log_network` in `fc3.toml` set. With it empty, the game hung at the profile login. The exact cause is not proven.
The value `127.0.0.1:28771` in this file has not been tested on a console.

On PC, the game stopped at the disc error screen without `patch.dat` and `patch.fat`. On the Switch: not verified.
