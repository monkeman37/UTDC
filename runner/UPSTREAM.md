# Upstream

`runner/` is a vendored copy of [Butterscotch](https://github.com/ButterscotchRunner/Butterscotch)
(AGPL-3.0) at commit `c4da68f6b0854ee9143c3556839322b1d0aee1fb`, plus the Dreamcast port.

Dreamcast-specific additions (new files):

- `src/dreamcast/` - platform layer: main loop, PowerVR2 renderer, VMU save system
- `src/audio/dreamcast/` - AICA audio backend (ADPCM sound effects + streamed music)
- `src/bytecode_cache.[ch]` - per-script compressed bytecode with an LRU decompression cache

Changes to upstream files (all small and platform-neutral or guarded):

- `CMakeLists.txt` - `PLATFORM=dreamcast` target
- `src/data_win.c` - large chunks (bytecode) take over the per-chunk load buffer instead of
  being copied; Dreamcast reopens data.win with a 32 KB stdio buffer for lazy room loads
- `src/utils.h`, `src/rvalue.h`, `src/vm_builtins.c`, `src/runner.c` - string constants from the
  data.win STRG chunk are stored by reference in ds_maps instead of being duplicated
- `src/vm.c` - bytecode is fetched through `BytecodeCache_acquire/release`
- `src/runner.c` - logs room transitions

To update: re-vendor a newer Butterscotch and re-apply the above (they are small).
The Cinnamon runner (Project Sunshine's Nintendo fork) was studied for reference only;
no code from it is included.

Upstream's `tests/` folder (regression data.win files and reference screenshots) is intentionally not included in this repository; use the Butterscotch repository for those.
