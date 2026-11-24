# Upstream

`runner/` is a vendored copy of [Butterscotch](https://github.com/ButterscotchRunner/Butterscotch)
(AGPL-3.0) at commit `c4da68f6b0854ee9143c3556839322b1d0aee1fb`, plus the Dreamcast port.

Dreamcast-specific additions (new files):

- `src/dreamcast/` - platform layer: main loop, PowerVR2 renderer, VMU save system, button-prompt text patches
- `src/audio/dreamcast/` - AICA audio backend (ADPCM sound effects + streamed music)
- `src/bytecode_cache.[ch]` - per-script compressed bytecode with an LRU decompression cache

Changes to upstream files (all small and platform-neutral or guarded):

- `CMakeLists.txt` - `PLATFORM=dreamcast` target
- `src/data_win.c` - large chunks (bytecode) take over the per-chunk load buffer instead of
  being copied; Dreamcast reopens data.win with a 32 KB stdio buffer for lazy room loads
- `src/utils.h`, `src/rvalue.h`, `src/vm_builtins.c`, `src/runner.c` - string constants from the
  data.win STRG chunk are stored by reference in ds_maps instead of being duplicated
- `src/vm.c` - bytecode is fetched through `BytecodeCache_acquire/release`
- `src/runner.c` - logs room transitions; on Dreamcast keeps the last three rooms' payloads parsed
  and times room payload loads
- `src/data_win.c` - lazy room payloads are read with one `fread` and parsed from memory
- `src/vm.c` - Int64 `Conv` opcodes are handled when `NO_RVALUE_INT64` is set (Undertale emits them)
- `src/vm_builtins.c` - `window_center` calls an optional `Runner.centerWindow` hook; `game_end` and
  `game_restart` log their caller; date/clock builtins go through `gmlNow()` (PC test builds can
  pin the time with `BS_FAKE_TIME`)
- `src/runner.h` - `centerWindow` hook
- `src/sweep.[ch]`, `src/loop.c` - automated room/battle/object coverage sweeps (test harness)
- `src/collision.h` - precise collision tests compute an instance's rotation once per test (and
  `Collision_pointInInstance` remembers the last angle's cos/sin); results are bit-identical
- `src/vm_builtins.c` - `sprite_create_from_screen` / `sprite_create_from_screen_x` (Undertale's
  lab camera) capture through the renderer's `createSpriteFromSurface` on the application surface

To update: re-vendor a newer Butterscotch and re-apply the above (they are small).
The Cinnamon runner (Project Sunshine's Nintendo fork) was studied for reference only;
no code from it is included.

Upstream's `tests/` folder (regression data.win files and reference screenshots) is intentionally not included in this repository; use the Butterscotch repository for those.
