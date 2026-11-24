# Testing

## Automated smoke test in Flycast

```sh
python3 tests/make_inputs.py tests/battle.txt tests/battle.json
builder/build-linux/utdc-builder-cli /path/to/Undertale out/UNDERTALE.gdi \
    --overlay --test-inputs tests/battle.json --exit-frame 3200
xvfb-run -a flycast out/UNDERTALE.gdi | grep -E "DC:|Runner: room"
```

`~/.config/flycast/emu.cfg` needs `Dreamcast.HLE_BIOS = yes` and
`Debug.SerialConsoleEnabled = yes` so the runner's log reaches stdout.

Every 5 s the runner prints a line like:

```
DC: frame 2239 fps 30 step 1.8ms draw 13.6ms polys 83 pages 9 (loads 15, evictions 6)
    vram 864KB heap 13187KB free 1907KB room room_floweybattle
```

- `fps` – frames completed in the last second (target 30)
- `step` – game logic time; `draw` – Draw events + waiting for the PVR
- `free` – RAM left for the game (keep above ~1 MB)

`--overlay` also draws bars on screen: green = 30 fps met, yellow = logic
time, blue = draw time, white tick = 33 ms budget.

## Scripted input

`tests/*.txt` use `wait N`, `tap KEY [N]`, `hold KEY N` (keys: Z X C ENTER UP
DOWN LEFT RIGHT). `make_inputs.py` turns them into Butterscotch's input
playback JSON. The same JSON runs on the PC CLI build for fast iteration:

```sh
butterscotch DATA.WIN --headless --lazy-rooms --lazy-textures --lazy-audio \
    --playback-inputs tests/battle.json --exit-at-frame 3200 --seed 1
```

## Results (Flycast, this build)

| Scene | fps | logic | RAM free |
|---|---|---|---|
| Intro story | 30 | 1.5 ms | 2.0 MB |
| Title / naming | 30 | 1.4–2.1 ms | 2.0 MB |
| Ruins rooms 1–2 | 30 | 1.7–2.0 ms | 1.9 MB |
| First battle | 30 | 1.8–2.2 ms | 1.9 MB |

Room transitions load 1–4 texture pages at ~16–37 ms each. VMU saves run in a
background thread (~1 s, no frame drops).

## Coverage sweeps (rooms, battles, objects)

`runner/src/sweep.c` drives the game automatically. On the PC CLI build it is
enabled with environment variables, on the Dreamcast with `--cfg` lines that the
builder writes to `UTDC.CFG`:

| PC (env) | Dreamcast (`--cfg`) | Meaning |
|---|---|---|
| `BS_SWEEP_MODE` | `sweep_mode=` | `rooms`, `battles` (sets `global.battlegroup`, enters `room_battle`) or `objects` (instance_create) |
| `BS_SWEEP_LIST` | `sweep_list=` | comma-separated room indices / battle groups / object indices |
| `BS_SWEEP_BOOT` | `sweep_boot=` | frame to start at (use with `tests/smoke.json` so a new game is running) |
| `BS_SWEEP_FRAMES` | `sweep_frames=` | frames per item |
| `BS_SWEEP_NOINPUT` | `sweep_noinput=1` | don't mash buttons |
| `BS_SWEEP_GOD` | `sweep_god=1` | keep HP full and the soul invincible in every mode (including Omega Flowey's own `my_hp`/`my_inv`), so a boss room cycles through all its attacks |
| `BS_SWEEP_CENSUS` | `sweep_census=N` | every N frames, log the six most numerous objects (`CENSUS f<frame> n=<count>: ...`) to attribute slow frames to attacks |
| `BS_SWEEP_DEBUG` | `sweep_debug=1` | set `global.debug` (the game's developer keys) |
| `BS_TEX_PROFILE` (DC) | `BS_TEX_PROFILE=N` | every N frames, log the texture pages the frame used and how much of each it actually drew from (`TEXPROF`) |
| `BS_NO_CATCHUP` (DC) | `BS_NO_CATCHUP=1` | disable catch-up frames (for comparisons) |
| `BS_SNAPSHOT_UNREADABLE` (DC) | `BS_SNAPSHOT_UNREADABLE=1` | treat framebuffer read-back captures as unreadable (tests the lab-monitor fallback) |
| `BS_FB_READBACK` (DC) | `BS_FB_READBACK=0` or `1` | skip the boot probe: force render-to-texture captures (0) or framebuffer read-back (1) |
| `BS_FB_THUMB` (DC) | `BS_FB_THUMB=first,last` | log a 16x12 brightness thumbnail of the displayed picture for each frame in the range (`THUMB`); needs Flycast's "Emulate framebuffer" |

While sweeping, CONFIRM/CANCEL/arrows are pressed pseudo-randomly and HP is
refilled in battles. The Omega Flowey sandbox (room 294, the whole attack cycle with
an invincible soul) is `--cfg sweep_mode=rooms --cfg sweep_list=294 --cfg sweep_boot=700
--cfg sweep_frames=30000 --cfg sweep_god=1 --cfg sweep_census=60`; on the PC build add
`--seed 7` for a repeatable run. Example (all 113 battle groups on the Dreamcast):

```sh
builder/build-linux/utdc-builder-cli Undertale out/UNDERTALE.gdi --test-inputs tests/smoke.json \
    --cfg sweep_mode=battles --cfg sweep_list=0,1,2,... --cfg sweep_boot=1000 --cfg sweep_frames=300
```

The Dreamcast log then reports, besides the 5 s stats line:
`DC: fps dip N`, `DC: slow frame`, `DC: room change took N ms`,
`DC: room payload X read in N ms`, `DC: out of VRAM for page`, `DC: window shake up to N px`,
`(catch-up N)` in the stats line (frames whose rendering was skipped to keep real-time pace),
`Runner: game_end()/game_restart() from <script>`.

`BS_FAKE_TIME="YYYY-MM-DD HH:MM"` pins the clock on PC builds to test date-based
events; the Dreamcast uses its real-time clock (set in the BIOS).
