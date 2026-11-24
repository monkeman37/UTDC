# UTDC — UNDERTALE for the Sega Dreamcast

A Dreamcast port of UNDERTALE built on a GameMaker runner re-implementation
([Butterscotch](https://github.com/ButterscotchRunner/Butterscotch)) running on
[KallistiOS](https://github.com/KallistiOS/KallistiOS).

**This repository contains no game files.** You build the disc image yourself
from your own installed copy of UNDERTALE (Steam/GOG, PC version 1.0x with
`data.win`) using the included wizard.

## Building the ROM

You need your own copy of UNDERTALE (the PC version: the folder that contains
`data.win` and the `.ogg` music files). In Steam: right-click UNDERTALE →
*Manage* → *Browse local files*. Downloads are on the
[Releases page](https://github.com/monkeman37/UTDC/releases).

### Windows

1. Download `UTDC-Wizard.exe`.
2. Run it and pick your UNDERTALE folder.
3. Pick where to save `UNDERTALE.gdi`.
4. Press *Build* (about a minute).

Prefer the command line? Use `utdc-builder-cli.exe "C:\...\Undertale" out\UNDERTALE.gdi`.

### Linux

1. Download `utdc-builder-cli-linux` (a static build, runs on any x86-64 distro).
2. Run:

   ```sh
   chmod +x utdc-builder-cli-linux
   ./utdc-builder-cli-linux ~/path/to/Undertale out/UNDERTALE.gdi
   ```

If you only have the native Linux release of UNDERTALE (`assets/game.unx`
instead of `data.win`), use the Windows files instead; in Steam you can get
them with *Properties → Compatibility → Force Proton*.

### macOS

There is no prebuilt macOS builder yet; compile the small builder yourself
(needs Xcode command line tools and CMake, e.g. `brew install cmake ninja`):

1. Download the source and `1ST_READ.BIN` + `IP.BIN` from the release into one folder, e.g. `~/utdc-payload`.
2. Build and run:

   ```sh
   cmake -S builder -B build -G Ninja -DUTDC_PAYLOAD_DIR=$HOME/utdc-payload
   ninja -C build
   ./build/utdc-builder-cli ~/path/to/Undertale out/UNDERTALE.gdi
   ```

The macOS build has not been tested yet.

### Playing it

You get `UNDERTALE.gdi` plus `track01.bin`, `track02.raw` and `track03.bin`
(~450 MB); keep them together. Open the `.gdi` in **Flycast** or **Redream**,
or copy the folder to a **GDEMU / MODE** SD card for a real Dreamcast.

## Controls

Matches the official console versions by button position:

| Dreamcast | Action |
|---|---|
| D-pad / analog stick | Move |
| A | Confirm |
| B | Cancel / hold to run |
| Y | Menu |
| X | Cancel (alternate) |
| L trigger | Confirm (alternate, as on console) |
| R trigger | Menu (alternate) |
| Start | Confirm / skip |
| A+B+X+Y+Start | Return to the Dreamcast BIOS |

A Dreamcast keyboard also works, with the same keys as the PC version
(Z/X/C, Enter, arrows), and can be used to type where the PC version expects
typing.

On-screen prompts (title screen, instruction screen, the in-game menu tutorial)
show Dreamcast buttons instead of keyboard keys. This is done by swapping
those text strings at load time (`runner/src/dreamcast/dc_text_patches.c`);
no game scripts are modified.

Saves go to the first VMU found (two alternating save files, ~2 blocks each, shown as "UNDERTALE DC"; the newest valid one is loaded, so switching off mid-save can't lose your previous save).
Writes happen in the background, so saving never stalls the game.

## What the wizard does

1. Reads `data.win` and the `.ogg` music next to it.
2. Cuts every sprite out of GameMaker's 2048×2048 texture pages and repacks
   them into small power-of-two, pre-twiddled ARGB1555/ARGB4444 pages
   (LZ4-compressed, sector-aligned) that the PowerVR2 can stream in and out
   of its ~5 MB of free VRAM, keeping each sprite's animation frames together.
   Photographic, many-coloured frames (Omega Flowey) become hardware VQ
   textures → `TEXTURES.BIN`.
3. Converts audio to AICA ADPCM at the original 44.1 kHz: sound effects in mono (long ones at the highest rate that fits the AICA 65,534-sample limit, or streamed), music in stereo
   streams → `AUDIO.BIN`.
4. Writes `data.win` without its texture/audio chunks → `DATA.WIN`, and
   copies `credits.txt` (the True Pacifist special-thanks names) if present.
5. Lays out a GD-ROM image (IP.BIN boot sector + ISO 9660 in the
   high-density area) with the Dreamcast runner as `1ST_READ.BIN`.

## Status

Tested in Flycast (dynarec, HLE BIOS): scripted play from boot through the
intro, naming screen, Ruins and the first battle, plus automated sweeps
through all 336 rooms and all 113 battle groups (see
[docs/TESTING.md](docs/TESTING.md)):

- **30 fps** in normal play (each frame held for two 60 Hz vblanks); short dips remain on
  some battle starts and while a killed monster turns to dust
- room changes ~30 ms, ≥1 MB RAM free, music/sound at the PC version's 44.1 kHz, VMU saves
- story events that close or restart the game, window shakes and clock-based events work

Known gaps (see [docs/STATUS.md](docs/STATUS.md)): drawing into GameMaker
user surfaces (only the static on one fake-crash screen), CDI output for
CD-Rs, and testing on real hardware / Redream.

## Building everything from source

Only needed for development. Linux or WSL:

```sh
toolchain/build-toolchain.sh          # SH-4 GCC 15 + KallistiOS (+ UTDC patch), ~1.5 h once
scripts/build-runner.sh               # -> runner/build-dc/1ST_READ.BIN, IP.BIN
cmake -S builder -B builder/build-linux -G Ninja && ninja -C builder/build-linux
cmake -S builder -B builder/build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
      && ninja -C builder/build-win   # -> UTDC-Wizard.exe (runner embedded)
```

GitHub Actions does all of this on every push (`.github/workflows/build.yml`).

## Repository layout

```
builder/      PC-side ROM builder: data.win reader, texture/audio converters,
              GD-ROM writer, Win32 wizard, CLI
common/       on-disc formats shared by builder and runner
runner/       Butterscotch + Dreamcast platform (src/dreamcast, src/audio/dreamcast)
scripts/      build-runner.sh
tests/        scripted-input smoke tests (make_inputs.py, *.txt)
toolchain/    KallistiOS toolchain build script + KOS patch
docs/         architecture, testing, status
```

## Testing

See [docs/TESTING.md](docs/TESTING.md): the builder can embed an input script
(`--test-inputs`) and an on-screen timing overlay (`--overlay`); the runner
logs fps, memory, texture-cache and room transitions over the serial port,
which Flycast prints to its console.

## License

AGPL-3.0-or-later (inherited from Butterscotch). See [THIRD_PARTY.md](THIRD_PARTY.md).
UNDERTALE is © Toby Fox. This project is not affiliated with or endorsed by
him, Sega, or the Butterscotch/Project Sunshine authors. Don't distribute
the generated disc images.
