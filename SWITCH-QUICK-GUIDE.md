# Far Cry 3 on Nintendo Switch — quick guide

## Before you start

This guide is for **v0.1.0-preview** (`fc3-switch-v0.1.0-preview.zip`).
Owner reports the release setup has been tested on their console; no independent clean-install validation across other hardware.
This is not a stable release, a verified full playthrough or a guarantee of 30 FPS.

Follow [INSTALL.md](INSTALL.md) for the supported game hashes and complete installation steps. You need your own
Xbox 360 game (Media ID `003B8D49`, `default.xex` version `0.0.0.4`) and matching Title Update 5.
Game disc files, update payloads, saves and a HOME forwarder are not included.

## What goes where

On your PC, extract the release archive. Its root includes `prepare_update.py`, the English guides, license notices
and `SHA256SUMS.txt`. Copy its `switch` folder to the SD card root, then add your own game and update files:

| SD path | Purpose |
|---|---|
| `switch/fc3-nx/fc3.nro` | Port executable |
| `switch/fc3-nx/fc3dk_shaders.dslib` | Matching shader library; keep beside the NRO |
| `switch/fc3-nx/fc3.toml` | Release settings |
| `switch/fc3-nx/game_root/` | Your extracted disc files, with `default.xex` directly inside |
| `switch/fc3-nx/update/` | Your locally prepared `patch.dat` and `patch.fat` |
| `switch/fc3-nx/fc3/` | Local profiles and saves created during use; back these up |

Create `game_root/` and `update/` if needed; their contents are not bundled.

**An ISO is not a ready-to-copy game folder.** Extract it with an Xbox 360-compatible tool, then copy the extracted
file tree's contents into `game_root/`. Merely unpacking an outer ZIP to obtain the ISO is not enough.
Keep the `.dat` and `.fat` archives intact, preserving subfolders such as `worlds/`. Do not unpack those archives.
`$SystemUpdate` is not needed.

Put your extracted `default.xex` and Title Update 5 file `tu00000004_00000000` beside `prepare_update.py` on your PC.
Run it without path arguments:

```text
python prepare_update.py
```

It finds the inputs beside itself, not in the terminal's working directory, and writes `update/patch.dat` and
`update/patch.fat` beside itself. Missing inputs are listed clearly. Copy the output pair into
`sdmc:/switch/fc3-nx/update/`, not inside `game_root/`. Existing output patch files are replaced on each run.
Do not apply `default.xexp`.

For optional Switch-style prompts, also put your own `common.dat` and `common.fat` beside `default.xex`, then run
`python prepare_update.py --icons`. Explicit input/output paths remain available; see [INSTALL.md](INSTALL.md).
Your original inputs remain unchanged. Generated patch files are for your own local use, not distribution.

## Launch and controls

1. Open hbmenu in **application mode with full memory**. A common method is to hold **R** while launching a game from
   the HOME menu and keep holding it until hbmenu appears.
2. Select Far Cry 3. Do not use Album/applet mode; it has insufficient memory. No forwarder is bundled or required.
3. With the Xbox button layout, **Switch B confirms** and **Switch A cancels** in-game. The Switch system UI instead
   uses **A** to confirm. Optional converted icons change prompts, not the physical mapping.

## Quick troubleshooting

- **Game files not found / disc error:** check that `game_root/default.xex` exists directly at that path, not inside
  another folder, and that the full extracted tree was copied. Check both `update/patch.dat` and `update/patch.fat`.
  Verify the edition and hashes in [INSTALL.md](INSTALL.md); an ISO's filename is not proof of compatibility.
- **Profile-login hang:** retain the supplied `log_network = "127.0.0.1:28771"` setting. This build needs a nonempty
  value for socket initialization. Loopback is not the owner's network destination; no listener or remote IP is needed.
  The release settings otherwise match the installed values.
- **No music:** enable it in the game's **main menu** options before loading a game. The music option is unavailable
  once a game is loaded.
- **Visual glitches or low frame rate:** lighting and texture issues remain in this preview. Measurements were made
  on one overclocked console, not across stock-clock hardware. This guide does not recommend changing clocks.
- **Replacing an older version:** close the game, back up your installation and saves, and keep the release's
  `fc3.nro`, `fc3dk_shaders.dslib` and `fc3.toml` together. Do not mix files from different builds.
