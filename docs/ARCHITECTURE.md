# Architecture

## Hardware budget

| Resource | Dreamcast | How UTDC fits |
|---|---|---|
| Main RAM | 16 MB | data.win without textures/audio (~16 MB on disc → ~10.6 MB parsed), rooms loaded lazily, bytecode deflated per script (5.0 MB → 1.2 MB + 1 MB cache), string constants shared instead of copied |
| VRAM | 8 MB | 640×480 double buffer + 640 KB vertex buffers; ~5.4 MB left for a texture-page LRU cache |
| Sound RAM | 2 MB | 3 music streams × 2 × 16 KB buffers; ~1.2 MB LRU cache of ADPCM sound effects |
| CPU | SH-4 200 MHz | float reals (`USE_FLOAT_REALS`), no int64 RValues, 30 fps game logic ≈ 2 ms/frame |

## Disc layout (GD-ROM, high-density area from LBA 45000)

```
IP.BIN        boot sector (makeip), 16 sectors
1ST_READ.BIN  runner (unscrambled; GD-ROM boot)
DATA.WIN      data.win minus TXTR/AUDO payloads
TEXTURES.BIN  repacked texture pages (common/utdc_formats.h)
AUDIO.BIN     ADPCM sound effects + music streams
UTDC.CFG      title / debug options
INPUT.JSN     (test builds only) scripted input
```

KallistiOS normally mounts the low-density track only; `toolchain/patches/`
makes `fs_iso9660` try the high-density area first (falls back for CD-R).

## Runner (runner/src/dreamcast)

- **main.c** – boot, BIOS-font loading screen, controller→keyboard mapping,
  vblank-locked frame pacing (`60 / room_speed` vblanks per frame), stats.
- **dc_renderer.c** – implements Butterscotch's `RendererVtable` on the PVR:
  everything goes to the translucent list with auto-sort disabled so polygons
  composite in GameMaker draw order; vertices go straight to the TA with
  `pvr_prim`. Texture pages are loaded on first use (one sector-aligned read,
  LZ4 block decode into RAM, 32-bit stores into VRAM) and evicted LRU, never
  evicting pages used by the current or previous frame.
- **dc_file_system.c** – GameMaker files live in RAM; any change is packed
  (deflate) into one VMU file and written by a background thread.
- **audio/dreamcast/dc_audio_system.c** – sound effects via the KOS sfx
  manager (AICA ADPCM, per-channel volume/pitch updates); music via
  `snd_stream` fed from disc by a polling thread.
- **bytecode_cache.c** – after `VM_create` patches the bytecode, every script
  is deflated separately and the blob is freed; `VM_executeCode` acquires a
  decompressed, pinned copy (LRU, 1 MB).

## Builder (builder/src)

- **datawin.c** – minimal chunk reader; writes the stripped DATA.WIN.
- **textures.c** – skyline packer, binary vs. soft alpha split (ARGB1555 vs
  ARGB4444), PVR twiddling, LZ4-HC 32 KB blocks.
- **audio.c** – WAV/OGG decode, polyphase resampler, AICA ADPCM encoder,
  stream keys so `audio_create_stream("music/x.ogg")` and `mus_x.ogg` match.
- **disc.c** – ISO 9660 writer with Mode-1 raw sectors (EDC/ECC) and the GDI
  track layout.
- **win_main.c** – the three-step wizard; **cli_main.c** – command line.
