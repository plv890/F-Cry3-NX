# Installation — v1.0.0

Owner reports the release setup has been tested on their console; no independent clean-install validation across other hardware.
A full playthrough has not been independently verified. Performance is not guaranteed at 30 FPS.

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

Use `F-Cry3-NX-v1.0.0.zip` from the
[F-Cry3-NX Releases](https://github.com/plv890/F-Cry3-NX/releases) page.
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

If you already have the extracted disc files, skip the extraction commands and check the folder layout below.
Otherwise, use your own Xbox 360 disc image. **Copying the `.iso` file onto the SD card does not work.**

### First: unpack an outer archive, if present

If your backup is inside a ZIP or 7z archive, an open-source archive tool such as **7-Zip** can unpack that outer
archive on your PC. Obtaining an `.iso` is only the first step: you must still extract its Xbox 360 game filesystem.
Mounting the ISO in Windows or seeing only a video/DVD partition is not enough.

### Then: extract the Xbox 360 ISO with one of these open-source tools

Choose **one** option below; you do not need to run both. These tools support the known Xbox 360 XGD2/XGD3 partition
offsets as well as partition-only XISO images. That does not guarantee that every dump is valid or supported.
Use the named upstream project, not an unrelated similarly named converter. The tools are not included in this release.

The examples use **Windows PowerShell**. Put the chosen executable and your ISO in a working folder, open that folder
in File Explorer, type `powershell` in the address bar and press Enter. Replace `Far Cry 3.iso` with your ISO's actual
filename, keeping the quotes. Choose a new, empty output folder.

**Option A — extract-xiso, maintained by XboxDev (open-source, BSD-style license):**

```powershell
.\extract-xiso.exe -x "Far Cry 3.iso" -d "FC3-extracted"
```

**Option B — xdvdfs-cli, by antangelo (open-source, MIT license; executable named xdvdfs):**

```powershell
.\xdvdfs.exe unpack "Far Cry 3.iso" "FC3-extracted"
```

If the downloaded executable has a different name, use that name in the command. To check your version's options,
run `.\extract-xiso.exe -h` or `.\xdvdfs.exe unpack -h`. On Linux/macOS, use the corresponding executable without
`.exe` (for example, `./xdvdfs unpack "Far Cry 3.iso" "FC3-extracted"`). Use extraction/unpack mode, not image creation
or rewriting. These examples were checked against upstream documentation and source; no new extraction of an actual
Far Cry 3 image was performed for this guide update.

### Check the extracted tree and copy its contents

The result should contain `default.xex`, `common.dat`, `common.fat`, the other disc files and the `worlds/` tree.
If these are missing, do not copy an incomplete result or rename another file to `default.xex`; check the extraction
result and image format first. The helper in the next step checks whether the extracted executable is the supported one.

Copy the **contents of `FC3-extracted/`**, preserving subfolders, into `switch/fc3-nx/game_root/` on the SD card.
The correct path is `switch/fc3-nx/game_root/default.xex`, **not**
`switch/fc3-nx/game_root/FC3-extracted/default.xex`. **Leave `.dat`/`.fat` archives intact; do not unpack those archives.**
`$SystemUpdate` is unnecessary and can be omitted.

## 3. Prepare your own Title Update 5 on the PC

1. Install **Python 3** on your PC. The helper uses only Python's standard library; no `pip` packages are needed.
2. Make a PC folder for preparation and put the following three files directly inside it. Copy `default.xex` from
   your extracted disc tree; keep the original in the tree you copy to the SD card. If your own title update is inside
   a ZIP/7z, unpack that outer archive first. The input here is the actual package `tu00000004_00000000`, not the ZIP
   or an already extracted `patch.dat`.

```text
FC3-preparation/
  prepare_update.py
  default.xex
  tu00000004_00000000
```

3. Open `FC3-preparation` in File Explorer, type `powershell` in the address bar and press Enter. Run the script
   **without any input/output path arguments**:

```powershell
python .\prepare_update.py
```

If Windows does not recognize `python` but the Python launcher is installed, use `py -3 .\prepare_update.py` instead.
Use a terminal rather than double-clicking the script so that any error message stays visible.
On Linux/macOS, open a terminal in the same folder and run `python3 prepare_update.py`.

The script automatically reads the two input files **beside the script itself**, even when invoked by its full path
from a different working directory. It writes `update/patch.dat` and `update/patch.fat` beside the script and prints
`Ready:` followed by the output location:

```text
FC3-preparation/
  prepare_update.py
  default.xex
  tu00000004_00000000
  update/
    patch.dat
    patch.fat
```
If a required file is missing, it lists the missing paths instead of asking you to enter a command format.
Existing files with these names in the output `update/` folder are replaced; your original game/update inputs are not changed.

4. After successful completion, copy **both** output files into `switch/fc3-nx/update/` on the SD card,
   **not inside `game_root/`**. Do not copy the whole `FC3-preparation` folder, the Python script or the original TU
   package onto the SD card. The disc's `default.xex` belongs in `game_root/`, as shown in the final layout below.

The helper checks the supported XEX hash and update IDs; an unexpected TU hash is reported as a warning.
If it reports `Missing required files`, check the listed paths and actual filenames (enable file extensions in File
Explorer). If it rejects the executable or update IDs, check your edition against the table above; do not bypass the
checks. A TU hash warning means it differs from the tested package, even if its IDs allow extraction.
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
