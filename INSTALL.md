# Installation — v0.1.0-preview

Owner reports the release setup has been tested on their console; no independent clean-install validation across other hardware.
This is a preview, not a stable release or a verified full playthrough. Performance is not guaranteed at 30 FPS.

## Supported game and required update

This port is built for one `default.xex`. The game and the title update must both have Media ID `003B8D49`.
An ISO filename alone does not establish compatibility.

| | |
|---|---|
| Game | Far Cry 3, Xbox 360, disc "Europe, Asia (En,Pl,Ru)" |
| Title ID | `5553088C` |
| Media ID | `003B8D49` |
| `default.xex` version | 0.0.0.4 |
| `default.xex` SHA-256 | `f9c08098c7e435f89899955045608f727c0d1bea13e4de84e06b93cd729539df` |
| Title update | Title Update 5, file `tu00000004_00000000`, 7,712,768 bytes |
| Title update SHA-256 | `367a9caae9b3957aec1bb7258b62ecaaf0efbf3230fd4749bbee2ba156d80c09` |

Title Update 5 for Media ID `6F6EAD36` belongs to another edition and does not fit. Other editions are not verified.
The game disc files and title update are not included. Use your own copies; do not redistribute extracted game data
or generated `patch.dat`/`patch.fat`, including files made with the optional icon conversion.

## 1. Extract the release archive

Use `fc3-switch-v0.1.0-preview.zip` from the private
[plv890/fc3-switch-release Releases](https://github.com/plv890/fc3-switch-release/releases) page, which requires repository access.
Extract it on your PC. Its archive-root contents are:

```text
prepare_update.py
README.md
INSTALL.md
SWITCH-QUICK-GUIDE.md
LICENSE
THIRD_PARTY_NOTICES.md
licenses/
SHA256SUMS.txt
switch/fc3-nx/
  fc3.nro
  fc3dk_shaders.dslib
  fc3.toml
```

`prepare_update.py` runs on the PC, not on the Switch. Use `SHA256SUMS.txt` to check the packaged files after extraction.
The NRO and shader library match the installed binaries:

| File | SHA-256 |
|---|---|
| `fc3.nro` | `582fefa992a34a1dfa12152885b923870a9bbe04b970e2edbd317ce832d142a7` |
| `fc3dk_shaders.dslib` | `c6fcfdac47f4cc14cb527122246039b74bfdc731660443ddc216ed206e2520ea` |

Copy the archive's `switch` folder to the SD card root. Create `switch/fc3-nx/game_root/` and
`switch/fc3-nx/update/` manually if needed. These folders' game/update payloads are not bundled.
Allow about 6 GB in total for the port and your game files. Back up an existing installation and saves before replacing files.

## 2. Extract your ISO, then copy the extracted files

Use an **Xbox 360-compatible ISO extraction tool** on your own disc image. If your backup is inside a ZIP or similar
archive, extracting that outer archive only gives you the ISO: you must still extract the Xbox 360 filesystem from it.
**Copying the `.iso` file onto the SD card does not work.**

Copy the **contents of the extracted disc tree**, preserving subfolders, into `switch/fc3-nx/game_root/`.
`default.xex` must be directly inside `game_root/`, not inside another enclosing folder. Include the game's `.dat`
and `.fat` files and `worlds/` tree. **Leave `.dat`/`.fat` archives intact; do not unpack those archives.**
`$SystemUpdate` is unnecessary and can be omitted.

## 3. Prepare your own Title Update 5 on the PC

Install Python 3. Put these three files together in a folder on your PC:

```text
prepare_update.py
default.xex                your supported, extracted game executable
tu00000004_00000000        your extracted Title Update 5 package, not its ZIP
```

Run the script **without any path arguments**:

```text
python prepare_update.py
```

The script automatically reads the two input files **beside the script itself**, even when started from a different
working directory. It writes `update/patch.dat` and `update/patch.fat` beside the script and prints the output location.
If a required file is missing, it lists the missing paths instead of asking you to enter a command format.
Existing files with these names in the output `update/` folder are replaced; your original game/update inputs are not changed.

Copy both output files into `switch/fc3-nx/update/` on the SD card, **not inside `game_root/`**.
The helper checks the supported XEX hash and update IDs; an unexpected TU hash is reported as a warning.
It does not download game files. Do not apply the update's `default.xexp` to `default.xex`.

Optional Switch-style button icons require your own `common.fat` and `common.dat` beside `default.xex`.
With all inputs beside the script, run:

```text
python prepare_update.py --icons
```

If you prefer to keep your inputs elsewhere, explicit paths remain supported:

```text
python "prepare_update.py" "my-game/default.xex" "my-tu/tu00000004_00000000" "prepared-update"
```

Add `--icons` to that command if needed; `common.fat` and `common.dat` must be beside the selected `default.xex`.
Converted icons change button prompts, not the physical mapping. Keep the generated patch pair for your own local use;
do not distribute it.

## 4. Check the final SD layout and launch

After adding your own files, the layout should include:

```text
sdmc:/switch/fc3-nx/
  fc3.nro
  fc3dk_shaders.dslib
  fc3.toml
  game_root/
    default.xex
    common.dat
    common.fat
    ... other extracted disc files and worlds/ ...
  update/
    patch.dat
    patch.fat
```

`sdmc:/` means the SD card root. The game/update entries above are supplied by you, not by the release archive.

Use a Switch configured to run homebrew. Launch **hbmenu in application mode with full memory**: commonly, hold **R**
while launching a game from HOME and keep holding it until hbmenu opens. Then select Far Cry 3.
Do not launch through Album/applet mode, which has insufficient memory. No HOME forwarder is bundled or required.

## Configuration and everyday use

- Keep `fc3.nro`, `fc3dk_shaders.dslib` and `fc3.toml` from the same release together.
- The supplied configuration matches the installed values except `log_network = "127.0.0.1:28771"`.
  **Leave this value nonempty**: this build uses it for socket initialization. Clearing it can cause a profile-login
  hang. This loopback address is not the owner's network destination. You do not need a logging listener or remote IP.
- With the Xbox layout, **Switch B confirms** and **Switch A cancels** in-game; the Switch system UI uses **A** to confirm.
- No music? Enable music in the game's **main menu** options, before loading a game. That option is unavailable once a game is loaded.
- Measurements came from one overclocked console; they are not a stock-clock performance claim or a recommendation
  to change clocks. Lighting and texture glitches remain possible.

See [SWITCH-QUICK-GUIDE.md](SWITCH-QUICK-GUIDE.md) for a short usage and troubleshooting reference.
