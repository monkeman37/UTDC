# Status and known gaps

## Working (verified in Flycast)

- Boot from GD-ROM image, loading screen, VMU detection
- Full asset pipeline: 6,550 texture items → 189 PVR pages, 279 sound effects, 164 music tracks
- Rendering: sprites (scaled/rotated/coloured), tiles, backgrounds, text with gradients, rectangles,
  lines, primitives, blend modes, views/ports
- Input (controller → keyboard emulation), naming screen, menus, overworld, battle
- Music streaming with looping, sound effects, VMU save/load in the background
- Locked 30 fps in every tested scene

## Gaps / TODO

| Area | Detail |
|---|---|
| Surfaces | `surface_set_target` draws are skipped and `draw_surface` is a no-op. Undertale uses one surface for a transition effect. A PVR render-to-texture pass could implement it. |
| Music pitch | `audio_sound_pitch` on a playing music stream applies the next time it starts (SFX pitch is live). Needs a KOS stream channel-frequency update. |
| CDI output | Only GDI is written. CD-R users need a DiscJuggler CDI (data/data, scrambled 1ST_READ.BIN, MSINFO-adjusted ISO). |
| Real hardware | Untested. GD-ROM seek times are much higher than in emulators; texture pages are sector-aligned and read in one request, but room loads may hitch longer. |
| Redream | Untested (GDI with raw 2352-byte data tracks should load). |
| Later areas | Only tested up to the first battle. RAM headroom is ~1.9 MB; areas with very large text tables or many instances need checking. |
| Keyboard | Dreamcast keyboard is detected but not mapped. |
| Wizard on Windows | Built with MinGW and not yet run on Windows; the same builder core is tested on Linux. |
