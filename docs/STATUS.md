# Status and known gaps

## Working (verified in Flycast)

- Boot from GD-ROM image, loading screen, VMU detection; boot to the intro takes ~10 s (data.win chunks are read with one sector-aligned request each; the per-script bytecode packing reuses one compressor)
- Full asset pipeline: 6,550 texture items → ~660 PVR pages (256x256, so a frame only keeps the parts of an atlas it uses in VRAM), 264 sound effects, 179 streams (music + long effects), all at the original 44.1 kHz
- Texture packing keeps every sprite's animation frames together. Photographic, many-coloured sprite frames (Omega Flowey's body parts and attacks; nothing else in the game qualifies) are stored as hardware VQ textures (1/8 of the VRAM, median 30 dB against the original, no visible difference in play); all pixel art, fonts and backgrounds stay lossless
- Texture pages live in a VRAM pool managed by the renderer: when a page doesn't fit, the cheapest run of neighbouring pages is evicted at once (fragmentation used to stall a frame for over 0.5 s in the Omega Flowey fight)
- Omega Flowey fight: 30 fps through the whole attack cycle in Flycast (worst one-second averages 26-28 fps), measured in a sandbox that keeps the soul invincible (see TESTING.md). It used to drop to 13-20 fps because each frame of the side stalks' 62-frame animation needed its own 256 KB texture
- Catch-up frames: when the game falls a full frame behind real time, the next frame runs its draw events without rendering (never twice in a row), so frame-timed music cues stay in step with the music (Omega Flowey's phases used to outlast their tracks)
- Music starts no longer stall the game: the first disc read and AICA prefill happen on the audio thread, and that thread reads the disc with the audio lock released (each start used to cost 20-40 ms on the game thread)
- Rotated precise collisions compute the rotation once per test instead of once per pixel (double-precision trig is done in software on the SH-4; Omega Flowey's vines cost up to 80 ms per frame)
- Screen captures (`sprite_create_from_surface` on the application surface: the True Lab elevator losing power, Flowey's defeated soul, Alphys's lab monitor, heat shimmer, screen waves, Muffet's border). At boot the runner renders a test colour and reads it back: on real hardware (and Flycast with "Emulate framebuffer") captures read the framebuffer directly. Emulators that keep the picture on the host GPU (Flycast's default, likely Redream) returned stale VRAM there, the boot loading screen; for them the renderer records each frame's polygons and captures with render-to-texture instead: a capture shows from the frame after it is made, and effects that capture every frame use a rolling half-size capture of the previous frame. If a capture still comes back as one flat colour, the lab monitor shows the game's own static picture (the Linux/macOS one)
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
- Room changes requested from game code skip drawing the old room once more (as GameMaker does); that frame used to stay on screen for the whole room load
- True Pacifist credits: the builder copies `credits.txt` (the special-thanks names the PC version reads at run time) from the game folder onto the disc
- Repeated warnings are logged at most three times each: the serial log blocks while it sends, and a burst of warnings used to stall the game
- Window movement (`window_set_position`, used by the pre-Omega-Flowey crash and the genocide ending) shakes the picture on screen
- Date/time events use the Dreamcast's clock (set it in the BIOS); the random seed comes from the clock and boot timing, so the FUN value differs between new games
- `sprite_create_from_surface(application_surface)` (screen snapshots used by the heat-shimmer, elevator and post-Flowey effects) copies the last displayed frame
- Dreamcast keyboard: works like the PC keyboard, including typing (Mettaton's essay)
- Button prompts: the game's own button macro (`\*Z`/`\*X`/`\*C`, used for every "press ... " hint in dialogue) shows [A]/[B]/[Y]; Papyrus's date HUD prompt and Toriel's "computer" phone line use the game's console-version text

## Gaps / TODO

| Area | Detail |
|---|---|
| Surfaces | `surface_set_target` draws are skipped and `draw_surface` is a no-op. Undertale draws into a user surface only in the fake-crash screen after the Flowey fight (random noise), which therefore stays black. Screen snapshots (`sprite_create_from_surface` / `sprite_create_from_screen`) are supported but read the previous frame. |
| CDI output | Only GDI is written. CD-R users need a DiscJuggler CDI (data/data, scrambled 1ST_READ.BIN, MSINFO-adjusted ISO). |
| Real hardware | Untested. GD-ROM seek times are much higher than in emulators; texture pages are sector-aligned and read in one request, but room loads may hitch longer. |
| Redream | Untested (GDI with raw 2352-byte data tracks should load). |
| Monster dust | Large monsters dissolving (e.g. Vulkin) still cost ~3 frames of 75-100 ms on the Dreamcast build (thousands of particle instances); catch-up frames keep the timing. |
| Texture loads | The first time a new attack or background appears, its texture pages are read from disc in that frame: single-frame hitches of 40-270 ms (largest at Asriel's first attacks, which use 1024x1024 pages). Catch-up frames keep the game's timing, but the picture still hitches once. |
| Later areas | Rooms and battles are swept automatically (entered directly, with random input), not played through in story order. Lowest free RAM seen in the battle sweep: ~1.1 MB. |
| Wizard on Windows | Built with MinGW and not yet run on Windows; the same builder core is tested on Linux. |
