# UTDC — UNDERTALE for the Sega Dreamcast

A Dreamcast port of UNDERTALE built on a GameMaker runner re-implementation
([Butterscotch](https://github.com/ButterscotchRunner/Butterscotch)) running on
[KallistiOS](https://github.com/KallistiOS/KallistiOS).

**This repository contains no game files.** You build the disc image yourself
from your own installed copy of UNDERTALE (Steam/GOG, PC version 1.0x with
`data.win`) using the included wizard.

## Quick start (Windows)

1. Download `UTDC-Wizard.exe` from the Releases page (or build it, see below).
2. Run it.
3. **1.** Pick your UNDERTALE folder (the one with `data.win`; in Steam:
   right-click UNDERTALE → *Manage* → *Browse local files*).
4. **2.** Pick where to save `UNDERTALE.gdi`.
5. **3.** Press *Build*. It takes about a minute and produces
   `UNDERTALE.gdi` + `track01.bin`, `track02.raw`, `track03.bin` (~330 MB).

Play it in **Redream** or **Flycast**, or copy the folder to a **GDEMU/MODE**
SD card for real hardware. Keep the `.gdi` next to its track files.

A command-line version is included too:
`utdc-builder-cli "C:\...\Undertale" out\UNDERTALE.gdi`

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

On-screen prompts (title screen, instruction screen, the in-game menu tutorial)
show Dreamcast buttons instead of keyboard keys. This is done by swapping
those text strings at load time (`runner/src/dreamcast/dc_text_patches.c`);
no game scripts are modified.

Saves go to the first VMU found (one file, ~2 blocks, shows as "UNDERTALE DC").
Writes happen in the background, so saving never stalls the game.

## What the wizard does

1. Reads `data.win` and the `.ogg` music next to it.
2. Cuts every sprite out of GameMaker's 2048×2048 texture pages and repacks
   them into small power-of-two, pre-twiddled ARGB1555/ARGB4444 pages
   (LZ4-compressed, sector-aligned) that the PowerVR2 can stream in and out
   of its ~5 MB of free VRAM → `TEXTURES.BIN`.
3. Converts sound effects to mono AICA ADPCM and music to 32 kHz stereo ADPCM
   streams → `AUDIO.BIN`.
4. Writes `data.win` without its texture/audio chunks → `DATA.WIN`.
5. Lays out a GD-ROM image (IP.BIN boot sector + ISO 9660 in the
   high-density area) with the Dreamcast runner as `1ST_READ.BIN`.

## Status

Tested in Flycast (dynarec, HLE BIOS) with scripted input from boot through
the intro, title, naming screen, the first Ruins rooms and the first battle:

- locked **30 fps** (each frame held for exactly two 60 Hz vblanks)
- ~2 ms game logic, ≤14 ms render per frame
- ~1.9 MB RAM free during play (of 16 MB), texture eviction working
- music streaming, sound effects, VMU saves

Known gaps (see [docs/STATUS.md](docs/STATUS.md)):

- GameMaker *surfaces* are not rendered (Undertale uses one, for a transition effect).
- Pitch changes on *music* take effect only when a track restarts (sound-effect pitch works).
- Only GDI output for now (GDEMU and emulators). CDI for burning CD-Rs is not done yet.
- Not yet tested on real hardware or in Redream; later areas of the game are untested.

## Building from source

Linux or WSL:

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
