# Status and known gaps

## Working (verified in Flycast)

- Boot from GD-ROM image, loading screen, VMU detection; boot to the intro takes ~10 s (data.win chunks are read with one sector-aligned request each; the per-script bytecode packing reuses one compressor)
- Full asset pipeline: 6,550 texture items → 584 PVR pages (256x256, so a frame only keeps the parts of an atlas it uses in VRAM), 264 sound effects, 179 streams (music + long effects), all at the original 44.1 kHz
- Rendering: sprites (scaled/rotated/coloured), tiles, backgrounds, text with gradients, rectangles,
  lines, primitives, blend modes, views/ports
- Input (controller → keyboard emulation), naming screen, menus, overworld, battle
- Music streaming with looping, sound effects; playback speed/pitch matches the PC version (no 22 kHz downsampling, no truncated long effects, live music pitch changes)
- Streams are decoded from ADPCM to PCM on the SH-4, so pause/resume (e.g. room music during a battle), pitch changes and loops are exact and one-shot streams play to their end; sound effects get 40 reserved AICA channels so frequent text blips no longer cut off longer effects (Flowey's laugh). All 443 sounds were played back automatically and checked against their length
- VMU saves: real SAVE point → reboot → Continue screen shows the file → Continue returns to the save point (tests/save.txt, tests/cont.txt). Saves alternate between two VMU files so an interrupted write never loses the previous save
- Locked 30 fps in every tested scene
- Boss fights profiled on the Dreamcast build (Toriel, Papyrus, Undyne/Undying, Mettaton quiz/EX/NEO, Muffet, Amalgamates, Asgore, Asriel, Glyde): 30 fps apart from single-frame texture-load hitches; >= 2.2 MB RAM free
- Music pitch changes are applied live on the AICA channels (Flowey's slowing music when you dodge his pellets used to restart the stream every frame)
- Many instances destroyed in one frame (monster dust) are removed from the object lists and collision grid in one pass
- Automated sweeps (see TESTING.md): all 336 rooms and all 113 battle groups run without VM errors on the PC build; all battle groups run on the Dreamcast build in Flycast
- Room changes: payloads are read in one request and the last three rooms (plus `room_battle`) stay parsed, so walking back or leaving a battle needs no disc access (was ~300 ms per change)
- Story `game_end()` calls (the "crash" before the Omega Flowey fight, the end of the Flowey fight, the genocide ending) save and relaunch the game, like restarting it on PC; A+B+X+Y+Start still returns to the BIOS
- `game_restart()` (e.g. naming the fallen human "Gaster") restarts the game
- Window movement (`window_set_position`, used by the pre-Omega-Flowey crash and the genocide ending) shakes the picture on screen
- Date/time events use the Dreamcast's clock (set it in the BIOS); the random seed comes from the clock and boot timing, so the FUN value differs between new games
- `sprite_create_from_surface(application_surface)` (screen snapshots used by the heat-shimmer, elevator and post-Flowey effects) copies the last displayed frame
- Dreamcast keyboard: works like the PC keyboard, including typing (Mettaton's essay)
- Button prompts: the game's own button macro (`\*Z`/`\*X`/`\*C`, used for every "press ... " hint in dialogue) shows [A]/[B]/[Y]; Papyrus's date HUD prompt and Toriel's "computer" phone line use the game's console-version text

## Gaps / TODO

| Area | Detail |
|---|---|
| Surfaces | `surface_set_target` draws are skipped and `draw_surface` is a no-op. Undertale draws into a user surface only in the fake-crash screen after the Flowey fight (random noise), which therefore stays black. Screen snapshots are supported (see above) but read the previous frame and cost CPU time. |
| CDI output | Only GDI is written. CD-R users need a DiscJuggler CDI (data/data, scrambled 1ST_READ.BIN, MSINFO-adjusted ISO). |
| Real hardware | Untested. GD-ROM seek times are much higher than in emulators; texture pages are sector-aligned and read in one request, but room loads may hitch longer. |
| Redream | Untested (GDI with raw 2352-byte data tracks should load). |
| Texture loads | The first time a new attack or background appears, its texture pages are read from disc in that frame: single-frame hitches of 40-270 ms (largest at Asriel's first attacks, which use 1024x1024 pages). |
| Later areas | Rooms and battles are swept automatically (entered directly, with random input), not played through in story order. Lowest free RAM seen in the battle sweep: ~1.1 MB. |
| Wizard on Windows | Built with MinGW and not yet run on Windows; the same builder core is tested on Linux. |
