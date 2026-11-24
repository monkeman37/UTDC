/* Butterscotch - Sega Dreamcast platform layer (KallistiOS).
 *
 * Boot flow:  /cd/DATA.WIN (stripped by the builder), TEXTURES.BIN and
 * AUDIO.BIN are read from the disc, saves live on the first VMU found.
 *
 * Frame pacing: Undertale runs at room_speed 30. The PVR renders at the
 * display's 60 Hz and every game frame is shown for two vblanks; logic for
 * frame N+1 runs while the PVR is still rasterising frame N.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <kos.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/sound/sound.h>
#include <arch/arch.h>
#include <malloc.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "runner.h"
#include "runner_keyboard.h"
#include "vm.h"
#include "data_win.h"
#include "utils.h"
#include "log.h"
#include "dc_renderer.h"
#include "dc_file_system.h"
#include "dc_audio_system.h"
#include "input_recording.h"
#include "bytecode_cache.h"
#include "dc_text_patches.h"
#include "stb_ds.h"
#include "sweep.h"
#include <dc/vblank.h>

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_MALLOCSTATS);

#define DISC_ROOT "/cd"
#define STICK_DEADZONE 48

/* ===[ logging ]=== */
static bool g_quietLogs = false;
/* Log output goes over the serial port and blocks while it is sent (~11 KB/s), so a burst of
   warnings stalls the game (the credits stalled for 224 ms every time a name appeared while
   credits.txt was missing). Each warning/error message format is printed at most 3 times. */
static bool logAllowed(const logType type, const char* format) {
    if (type != LOG_TYPE_WARNING && type != LOG_TYPE_ERROR) return true;
    static const char* seen[256];
    static uint8_t count[256];
    uint32_t h = (uint32_t) (uintptr_t) format;
    h = (h ^ (h >> 9)) & 255u;
    for (uint32_t probe = 0; probe < 256; probe++) {
        uint32_t k = (h + probe) & 255u;
        if (seen[k] == format) {
            if (count[k] >= 3) return false;
            if (++count[k] == 3) dbglog(DBG_INFO, "W: (further messages like the next one are not logged)\n");
            return true;
        }
        if (!seen[k]) { seen[k] = format; count[k] = 1; return true; }
    }
    return true;
}

void platformLog(const logType type, const char* format, va_list va) {
    if (g_quietLogs && type == LOG_TYPE_DEBUG) return;
    if (!logAllowed(type, format)) return;
    char buf[512];
    vsnprintf(buf, sizeof buf, format, va);
    const char* prefix = type == LOG_TYPE_WARNING ? "W: " : type == LOG_TYPE_ERROR ? "E: " : type == LOG_TYPE_DEBUG ? "D: " : "";
    dbglog(DBG_INFO, "%s%s", prefix, buf);
}

/* ===[ pre-PVR text screen using the BIOS font ]=== */
static void textScreen(const char* title, const char* line1, const char* line2, float progress) {
    uint16_t* fb = vram_s;
    for (int i = 0; i < 640 * 480; i++) fb[i] = 0;
    bfont_set_foreground_color(0xFFFFFFFF);
    if (title) bfont_draw_str(fb + 640 * 160 + 40, 640, 0, title);
    if (line1) bfont_draw_str(fb + 640 * 220 + 40, 640, 0, line1);
    if (line2) bfont_draw_str(fb + 640 * 250 + 40, 640, 0, line2);
    if (progress >= 0) {
        int w = (int) (560 * progress);
        for (int y = 300; y < 312; y++) for (int x = 0; x < 560; x++) fb[y * 640 + 40 + x] = x < w ? 0xFFFF : 0x4208;
    }
}

static void fatal(const char* line1, const char* line2) {
    logError("%s %s\n", line1, line2 ? line2 : "");
    textScreen("UNDERTALE for Dreamcast", line1, line2, -1);
    for (;;) thd_sleep(1000);
}

static const char* g_loadTitle = "Loading";
static uint64_t g_bootT0, g_bootLast;
/* boot profiling: logs time spent since the previous mark */
static void bootMark(const char* what) {
    uint64_t now = timer_us_gettime64();
    logInfo("DC: boot %-24s %5u ms (total %u ms)\n", what, (unsigned) ((now - g_bootLast) / 1000), (unsigned) ((now - g_bootT0) / 1000));
    g_bootLast = now;
}
static void loadProgress(const char* chunkName, int chunkIndex, int totalChunks, MAYBE_UNUSED DataWin* dw, MAYBE_UNUSED void* user) {
    char line[64];
    snprintf(line, sizeof line, "Reading game data (%s)", chunkName);
    { char m[32]; snprintf(m, sizeof m, "before chunk %s", chunkName); bootMark(m); }
    textScreen(g_loadTitle, line, nullptr, totalChunks > 0 ? (float) chunkIndex / (float) totalChunks : 0);
}

/* ===[ controller -> keyboard mapping ]===
 * Matches the official console versions by button position:
 *   A (bottom) = CONFIRM (Z)    B (right) = CANCEL (X)    Y (top) = MENU (C)
 *   X (left)   = CANCEL (X)     L trigger = CONFIRM (Z)   R trigger = MENU (C)
 *   Start      = ENTER (confirm / skip), D-pad & analog stick = arrows */
typedef struct { uint32_t mask; int32_t key; } PadMap;
static const PadMap PAD_MAP[] = {
    { CONT_A, 'Z' }, { CONT_B, 'X' }, { CONT_X, 'X' }, { CONT_Y, 'C' },
    { CONT_START, VK_ENTER },
    { CONT_DPAD_UP, VK_UP }, { CONT_DPAD_DOWN, VK_DOWN }, { CONT_DPAD_LEFT, VK_LEFT }, { CONT_DPAD_RIGHT, VK_RIGHT },
};
#define VIRT_LTRIG (1u << 30)
#define VIRT_RTRIG (1u << 31)

typedef struct { int32_t key; int count; } KeyRef;
static uint8_t g_keyRefs[256];

static void keySet(Runner* runner, int32_t key, bool down, bool wasDown) {
    if (down == wasDown) return;
    if (key < 0 || key > 255) return;
    if (down) { if (g_keyRefs[key]++ == 0) RunnerKeyboard_onKeyDown(runner->keyboard, key); }
    else { if (g_keyRefs[key] > 0 && --g_keyRefs[key] == 0) RunnerKeyboard_onKeyUp(runner->keyboard, key); }
}

/* Dreamcast keyboard scan code -> GameMaker virtual key */
static int32_t kbdToVk(int code) {
    if (code >= KBD_KEY_A && code <= KBD_KEY_Z) return 'A' + (code - KBD_KEY_A);
    if (code >= KBD_KEY_1 && code <= KBD_KEY_9) return '1' + (code - KBD_KEY_1);
    if (code == KBD_KEY_0) return '0';
    if (code >= KBD_KEY_F1 && code <= KBD_KEY_F1 + 11) return 112 + (code - KBD_KEY_F1);
    switch (code) {
        case KBD_KEY_ENTER: case KBD_KEY_PAD_ENTER: return VK_ENTER;
        case KBD_KEY_ESCAPE: return 27;
        case KBD_KEY_BACKSPACE: return 8;
        case KBD_KEY_TAB: return 9;
        case KBD_KEY_SPACE: return 32;
        case KBD_KEY_UP: return VK_UP;
        case KBD_KEY_DOWN: return VK_DOWN;
        case KBD_KEY_LEFT: return VK_LEFT;
        case KBD_KEY_RIGHT: return VK_RIGHT;
        case KBD_KEY_DEL: return 46;
        default: return -1;
    }
}

static uint32_t g_prevButtons = 0;
static bool g_prevStick[4];

static bool pollInput(Runner* runner) {
    maple_device_t* dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    uint32_t buttons = 0;
    bool stick[4] = { false, false, false, false };
    if (dev) {
        cont_state_t* st = (cont_state_t*) maple_dev_status(dev);
        if (st) {
            buttons = st->buttons;
            if (st->ltrig > 128) buttons |= VIRT_LTRIG;
            if (st->rtrig > 128) buttons |= VIRT_RTRIG;
            stick[0] = st->joyy < -STICK_DEADZONE;  /* up */
            stick[1] = st->joyy > STICK_DEADZONE;   /* down */
            stick[2] = st->joyx < -STICK_DEADZONE;  /* left */
            stick[3] = st->joyx > STICK_DEADZONE;   /* right */
            /* A+B+X+Y+Start: return to the Dreamcast BIOS menu (standard soft reset) */
            uint32_t reset = CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START;
            if ((buttons & reset) == reset) return false;
        }
    }
    for (size_t i = 0; i < sizeof PAD_MAP / sizeof PAD_MAP[0]; i++)
        keySet(runner, PAD_MAP[i].key, (buttons & PAD_MAP[i].mask) != 0, (g_prevButtons & PAD_MAP[i].mask) != 0);
    keySet(runner, 'Z', (buttons & VIRT_LTRIG) != 0, (g_prevButtons & VIRT_LTRIG) != 0);
    keySet(runner, 'C', (buttons & VIRT_RTRIG) != 0, (g_prevButtons & VIRT_RTRIG) != 0);
    static const int32_t stickKeys[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    for (int i = 0; i < 4; i++) { keySet(runner, stickKeys[i], stick[i], g_prevStick[i]); g_prevStick[i] = stick[i]; }
    g_prevButtons = buttons;

    /* Dreamcast keyboard (optional): works like the PC keyboard, including typing
       (keyboard_string) for things like Mettaton's essay question. */
    maple_device_t* kbd = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    static bool kbPrev[KBD_MAX_KEYS];
    if (kbd) {
        kbd_state_t* ks = (kbd_state_t*) maple_dev_status(kbd);
        if (ks) {
            for (int code = 0; code < KBD_MAX_KEYS; code++) {
                int32_t vk = kbdToVk(code);
                if (vk < 0) continue;
                bool down = ks->key_states[code].is_down;
                keySet(runner, vk, down, kbPrev[code]);
                kbPrev[code] = down;
            }
            uint32_t mods = ks->last_modifiers.raw;
            static bool shiftPrev, ctrlPrev;
            bool sh = (mods & KBD_MOD_SHIFT) != 0, ct = (mods & KBD_MOD_CTRL) != 0;
            keySet(runner, VK_SHIFT, sh, shiftPrev); shiftPrev = sh;
            keySet(runner, VK_CONTROL, ct, ctrlPrev); ctrlPrev = ct;
        }
        int k;
        while ((k = kbd_queue_pop(kbd, true)) != KBD_QUEUE_END) {
            if (k > 0 && k < 256) RunnerKeyboard_onCharacter(runner->keyboard, (unsigned) k);
            else if ((k >> 8) == KBD_KEY_BACKSPACE) RunnerKeyboard_onCharacter(runner->keyboard, 8);
        }
    } else {
        for (int code = 0; code < KBD_MAX_KEYS; code++) {
            if (kbPrev[code]) { int32_t vk = kbdToVk(code); if (vk >= 0) keySet(runner, vk, false, true); kbPrev[code] = false; }
        }
    }
    return true;
}

/* ===[ memory ]=== */
static uint32_t freeRamKB(void) {
    struct mallinfo mi = mallinfo();
    uintptr_t brk = (uintptr_t) sbrk(0);
    uintptr_t top = (uintptr_t) _arch_mem_top;
    uintptr_t stackReserve = 64 * 1024; /* KOS keeps the main stack at the top of RAM */
    uintptr_t untouched = top > brk + stackReserve ? top - brk - stackReserve : 0;
    return (uint32_t) ((untouched + (uintptr_t) mi.fordblks) / 1024);
}

/* ===[ vblank counter for frame pacing ]=== */
static volatile uint32_t g_vblanks = 0;
static void onVblank(MAYBE_UNUSED uint32_t code, MAYBE_UNUSED void* data) { g_vblanks++; }

/* ===[ debug overlay ]=== */
typedef struct { float frameMs, stepMs, drawMs; int fps; } FrameStats;

static void drawOverlay(Renderer* renderer, const FrameStats* fs) {
    /* fps bar: green = 30 fps target met */
    float w = (float) fs->fps * 10.0f;
    DcRenderer_screenRect(renderer, 4, 4, 4 + 300, 10, 0x80000000);
    DcRenderer_screenRect(renderer, 4, 4, 4 + w, 10, fs->fps >= 30 ? 0xC000FF00 : 0xC0FF2000);
    float sw = fs->stepMs * 6.0f, dw = fs->drawMs * 6.0f;
    DcRenderer_screenRect(renderer, 4, 12, 4 + sw, 16, 0xC0FFFF00);
    DcRenderer_screenRect(renderer, 4 + sw, 12, 4 + sw + dw, 16, 0xC000A0FF);
    DcRenderer_screenRect(renderer, 4 + 200, 12, 4 + 201, 18, 0xFFFFFFFF); /* 33.3 ms mark */
}

/* ===[ emulated PC window ]===
 * Some scenes move the game window around the desktop with window_set_position (the
 * "crash" before Omega Flowey, the genocide ending shake). We pretend the window sits
 * centred on a 1920x1080 desktop and shift the whole picture by the same amount. */
#define WIN_BASE_X ((1920 - 640) / 2)
#define WIN_BASE_Y ((1080 - 480) / 2)
#define WIN_MAX_SHIFT 160
static Renderer* g_renderer;
static int32_t g_winX = WIN_BASE_X, g_winY = WIN_BASE_Y;
static bool dcGetWindowPosition(int32_t* x, int32_t* y) { *x = g_winX; *y = g_winY; return true; }
static int32_t g_winMaxShift; /* largest shift since the last stats line (logged) */
static void dcSetWindowPosition(int32_t x, int32_t y) {
    g_winX = x; g_winY = y;
    int32_t m = abs(x - WIN_BASE_X) > abs(y - WIN_BASE_Y) ? abs(x - WIN_BASE_X) : abs(y - WIN_BASE_Y);
    if (m > g_winMaxShift) g_winMaxShift = m;
    int32_t dx = x - WIN_BASE_X, dy = y - WIN_BASE_Y;
    if (dx < -WIN_MAX_SHIFT) dx = -WIN_MAX_SHIFT;
    if (dx > WIN_MAX_SHIFT) dx = WIN_MAX_SHIFT;
    if (dy < -WIN_MAX_SHIFT) dy = -WIN_MAX_SHIFT;
    if (dy > WIN_MAX_SHIFT) dy = WIN_MAX_SHIFT;
    DcRenderer_setWindowOffset(g_renderer, (float) dx, (float) dy);
}
static void dcCenterWindow(void) { dcSetWindowPosition(WIN_BASE_X, WIN_BASE_Y); }

int main(MAYBE_UNUSED int argc, MAYBE_UNUSED char** argv) {
    vid_set_mode(DM_640x480, PM_RGB565);
    g_bootT0 = g_bootLast = timer_us_gettime64();
    textScreen("UNDERTALE for Dreamcast", "Starting...", nullptr, 0);

    /* log what the CD driver sees (helps diagnose bad disc images) */
    {
        file_t d = fs_open(DISC_ROOT, O_RDONLY | O_DIR);
        if (d == FILEHND_INVALID) logError("DC: cannot open %s\n", DISC_ROOT);
        else {
            dirent_t* de;
            while ((de = fs_readdir(d)) != nullptr) logDebug("DC: disc: %s (%d bytes)\n", de->name, (int) de->size);
            fs_close(d);
        }
    }

    /* settings written by the builder */
    char title[64] = "UNDERTALE";
    bool showOverlay = false;
    int exitFrame = -1;
    {
        file_t f = fs_open(DISC_ROOT "/UTDC.CFG", O_RDONLY);
        if (f != FILEHND_INVALID) {
            char buf[2048];
            ssize_t n = fs_read(f, buf, sizeof buf - 1);
            fs_close(f);
            if (n > 0) {
                buf[n] = 0;
                char* line = strtok(buf, "\r\n");
                while (line) {
                    if (!strncmp(line, "title=", 6)) snprintf(title, sizeof title, "%s", line + 6);
                    else if (!strncmp(line, "overlay=", 8)) showOverlay = atoi(line + 8) != 0;
                    else if (!strncmp(line, "exitframe=", 10)) exitFrame = atoi(line + 10);
                    else if (!strncmp(line, "BS_", 3) && strchr(line, '=')) {
                        /* test harness: BS_*=value lines become environment variables */
                        char key[48]; const char* eq = strchr(line, '=');
                        if (eq - line < (int) sizeof key) { snprintf(key, sizeof key, "%.*s", (int) (eq - line), line); setenv(key, eq + 1, 1); }
                    }
                    else if (!strncmp(line, "sweep_", 6)) {
                        /* test harness: sweep_mode=, sweep_list=, ... -> BS_SWEEP_* (see sweep.c) */
                        char key[32]; const char* eq = strchr(line, '=');
                        if (eq && eq - line < 24) {
                            snprintf(key, sizeof key, "BS_SWEEP_%.*s", (int) (eq - line - 6), line + 6);
                            for (char* k = key; *k; k++) if (*k >= 'a' && *k <= 'z') *k -= 32;
                            setenv(key, eq + 1, 1);
                        }
                    }
                    line = strtok(nullptr, "\r\n");
                }
            }
        }
    }
    g_loadTitle = title;

    /* ===[ data.win ]=== */
    DataWinParserOptions options = {0};
    options.parseGen8 = options.parseOptn = options.parseLang = options.parseExtn = true;
    options.parseSond = options.parseAgrp = options.parseSprt = options.parseBgnd = true;
    options.parsePath = options.parseScpt = options.parseGlob = options.parseShdr = true;
    options.parseFont = options.parseTmln = options.parseObjt = options.parseRoom = true;
    options.parseTpag = options.parseCode = options.parseVari = options.parseFunc = options.parseStrg = true;
    options.parseTxtr = false;
    options.parseAudo = false;
    options.skipLoadingPreciseMasksForNonPreciseSprites = true;
    options.lazyLoadRooms = true;
    /* The battle room is entered constantly and is tiny: keep it parsed. */
    shput(options.eagerlyLoadedRooms, "room_battle", true);
    options.loadType = DATAWINLOADTYPE_LOAD_PER_CHUNK;
    options.progressCallback = loadProgress;

    bootMark("startup");
    DataWin* dataWin = DataWin_parse(DISC_ROOT "/DATA.WIN", options);
    if (!dataWin) fatal("Could not read DATA.WIN from the disc.", "Rebuild the image with the UTDC wizard.");
    bootMark("data.win parsed");
    DcTextPatches_apply(dataWin);
    { extern const char* (*g_dsMapAddAlias)(const char* key); g_dsMapAddAlias = DcTextPatches_aliasFor; }
    {
        struct mallinfo mi = mallinfo();
        logInfo("DC: data.win loaded, heap in use %d KB\n", mi.uordblks / 1024);
    }

    textScreen(title, "Preparing...", nullptr, 1.0f);

    /* ===[ PVR ]=== */
    pvr_init_params_t params;
    memset(&params, 0, sizeof params);
    params.opb_sizes[PVR_LIST_OP_POLY] = PVR_BINSIZE_0;
    params.opb_sizes[PVR_LIST_OP_MOD] = PVR_BINSIZE_0;
    params.opb_sizes[PVR_LIST_TR_POLY] = PVR_BINSIZE_32;
    params.opb_sizes[PVR_LIST_TR_MOD] = PVR_BINSIZE_0;
    params.opb_sizes[PVR_LIST_PT_POLY] = PVR_BINSIZE_0;
    params.vertex_buf_size = 384 * 1024; /* peak use is logged ("TA vertex buffer max") */
    params.dma_enabled = 0;
    params.fsaa_enabled = 0;
    params.autosort_disabled = 1;
    params.opb_overflow_count = 2;
    if (pvr_init(&params) < 0) fatal("Could not initialise the PowerVR.", nullptr);
    pvr_set_bg_color(0, 0, 0);

    bootMark("pvr init");
    Renderer* renderer = DcRenderer_create(DISC_ROOT "/TEXTURES.BIN");
    if (!renderer) fatal("Could not read TEXTURES.BIN from the disc.", nullptr);

    FileSystem* fileSystem = DcFileSystem_create(dataWin->gen8.displayName, DISC_ROOT);

    bootMark("renderer + VMU");
    snd_init();
    AudioSystem* audioSystem = DcAudioSystem_create(DISC_ROOT "/AUDIO.BIN");
    if (!audioSystem) fatal("Could not read AUDIO.BIN from the disc.", nullptr);

    bootMark("audio");
    VMContext* vm = VM_create(dataWin);
    textScreen(title, "Packing game code...", nullptr, 1.0f);
    /* Deflate the patched bytecode per script and keep a 1 MB cache of decompressed scripts:
       saves roughly 3-4 MB of the Dreamcast's 16 MB. */
    bootMark("VM create");
    BytecodeCache_compressAll(dataWin, 1024 * 1024, 3);
    /* Like GameMaker on PC, start with a random seed (clock + boot timing), so values such as
       the FUN value rolled at the start of a new game differ between playthroughs. */
    uint32_t bootSeed = ((uint32_t) time(NULL) * 2654435761u) ^ (uint32_t) timer_us_gettime64();
    bootMark("bytecode packed");
    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem, bootSeed);
    bootMark("runner create");
    DcRenderer_probeFramebuffer(renderer);
    {
        struct mallinfo mi = mallinfo();
        logInfo("DC: runner created, heap in use %d KB, free RAM %u KB, VRAM free %d KB\n", mi.uordblks / 1024, freeRamKB(), (int) (pvr_mem_available() / 1024));
    }

    /* Automated test runs: replay scripted input from the disc if present */
    InputRecording* playback = nullptr;
    {
        file_t f = fs_open(DISC_ROOT "/INPUT.JSN", O_RDONLY);
        if (f != FILEHND_INVALID) {
            fs_close(f);
            playback = InputRecording_createPlayer(DISC_ROOT "/INPUT.JSN", nullptr);
            logInfo("DC: input playback enabled\n");
        }
    }

    g_renderer = renderer;
    runner->getWindowPosition = dcGetWindowPosition;
    runner->setWindowPosition = dcSetWindowPosition;
    runner->centerWindow = dcCenterWindow;
    {
        time_t now = time(NULL);
        struct tm* lt = localtime(&now);
        if (lt) logInfo("DC: system clock %04d-%02d-%02d %02d:%02d:%02d\n", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
    }

    vblank_handler_add(onVblank, nullptr);
    Runner_initFirstRoom(runner);
    bootMark("first room");

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t) gen8->defaultWindowWidth;
    int32_t gameH = (int32_t) gen8->defaultWindowHeight;

    FrameStats stats = {0};
    uint64_t lastFrame = timer_us_gettime64();
    uint64_t fpsWindowStart = lastFrame;
    int fpsFrames = 0;
    uint64_t frameCounter = 0;
    uint64_t lastLog = lastFrame;
    uint32_t lastFlipVbl = g_vblanks;
    /* real-time catch-up state (see the null frame below); BS_NO_CATCHUP=1 disables it */
    const bool catchUp = getenv("BS_NO_CATCHUP") == nullptr;
    int64_t lagUs = 0;
    bool lastWasNull = false;
    uint32_t nullFrames = 0;
    uint64_t prevFrameStart = 0;

    while (!runner->shouldExit) {
        uint64_t frameStart = timer_us_gettime64();
        runner->deltaTime = (GMLReal) (frameStart - lastFrame);
        lastFrame = frameStart;
        uint32_t roomSpeedNow = runner->currentRoom && runner->currentRoom->speed ? runner->currentRoom->speed : 30;
        int64_t frameUs = 1000000 / (int64_t) roomSpeedNow;
        if (prevFrameStart) {
            int64_t took = (int64_t) (frameStart - prevFrameStart);
            if (took > 250000) lagUs = 0;                       /* loading hitch: don't fast-forward */
            else { lagUs += took - frameUs; if (lagUs < 0) lagUs = 0; if (lagUs > 3 * frameUs) lagUs = 3 * frameUs; }
        }
        prevFrameStart = frameStart;

        if (!pollInput(runner)) break;
        if (playback) InputRecording_processFrame(playback, runner->keyboard, runner->frameCount);
        if (exitFrame >= 0 && runner->frameCount >= exitFrame) {
            /* automated tests: finish any VMU write, then idle so the harness can stop the emulator safely */
            DcFileSystem_flush(fileSystem);
            DcFileSystem_waitIdle(fileSystem);
            logInfo("DC: test finished at frame %d\n", runner->frameCount);
            for (;;) thd_sleep(1000);
        }

        uint64_t t0 = timer_us_gettime64();
        Runner_step(runner);
        uint64_t t1 = timer_us_gettime64();

        /* Catch-up: when the game has fallen a full frame behind real time, this frame runs its
           draw events without rendering, so game logic (and every frame-timed music cue, e.g.
           Omega Flowey's phase changes) keeps real-time pace through a heavy moment. Never two
           in a row, and loading hitches (room changes, >250 ms frames) are forgiven rather than
           fast-forwarded. */
        /* GameMaker switches rooms straight after the step that called room_goto and doesn't
           draw the old room again. Drawing it here showed a stray frame of the old room (often
           half set up for the transition) for as long as the room change took. */
        bool roomChanging = runner->pendingRoom != -1;
        bool nullFrame = !roomChanging && catchUp && lagUs >= frameUs && !lastWasNull;
        if (!roomChanging) {
            if (nullFrame) { DcRenderer_beginNullScene(renderer); nullFrames++; }
            else DcRenderer_beginScene(renderer);
            lastWasNull = nullFrame;
            Runner_drawPre(runner, 640, 480);
            Runner_beginFrame(runner, gameW, gameH, 640, 480, 640, 480);
            Runner_drawViews(runner, gameW, gameH, false);
            runner->viewCurrent = 0;
            renderer->vtable->endFrameInit(renderer);
            Runner_drawPost(runner, 640, 480);
            renderer->vtable->endFrameEnd(renderer);
            Runner_drawGUI(runner, 640, 480, gameW, gameH);
            if (showOverlay) drawOverlay(renderer, &stats);
            DcRenderer_endScene(renderer);
        }
        uint64_t t2 = timer_us_gettime64();

        RunnerKeyboard_beginFrame(runner->keyboard);

        uint32_t roomSpeed = runner->currentRoom ? runner->currentRoom->speed : 30;
        if (roomSpeed == 0) roomSpeed = 30;
        audioSystem->vtable->update(audioSystem, 1.0f / (float) roomSpeed);
        DcFileSystem_tick(fileSystem);

        Sweep_tick(runner);
        {
            /* debug: BS_FB_DUMP_FRAME=N prints the displayed framebuffer (RGB565, hex) to the log */
            static int dumpFrame = -2;
            if (dumpFrame == -2) { const char* e = getenv("BS_FB_DUMP_FRAME"); dumpFrame = e ? atoi(e) : -1; }
            /* debug: BS_FB_THUMB=first,last logs a 16x12 brightness thumbnail of the displayed
               picture for each frame in that range (transition analysis) */
            static int thumbA = -2, thumbB = -1;
            if (thumbA == -2) { const char* e = getenv("BS_FB_THUMB"); thumbA = -1; if (e) sscanf(e, "%d,%d", &thumbA, &thumbB); }
            if (thumbA >= 0 && runner->frameCount >= thumbA && runner->frameCount <= thumbB) {
                uint32_t off = PVR_GET(PVR_FB_ADDR) & 0x007FFFFFu;
                const volatile uint16_t* fb = (const volatile uint16_t*) (PVR_RAM_BASE + off);
                char line[16 * 12 + 1];
                for (int ty = 0; ty < 12; ty++)
                    for (int tx = 0; tx < 16; tx++) {
                        uint16_t p = fb[(ty * 40 + 20) * 640 + tx * 40 + 20];
                        int l = (((p >> 11) & 31) * 2 + ((p >> 5) & 63) + (p & 31) * 2) * 15 / 252;
                        line[ty * 16 + tx] = "0123456789abcdef"[l > 15 ? 15 : l];
                    }
                line[16 * 12] = 0;
                logInfo("THUMB %d %s room %s\n", runner->frameCount, line, runner->currentRoom ? runner->currentRoom->name : "?");
            }
            if (dumpFrame >= 0 && runner->frameCount == dumpFrame) {
                uint32_t off = PVR_GET(PVR_FB_ADDR) & 0x007FFFFFu;
                logInfo("FBINFO addr=%08lx il=%08lx vram_s=%p cfg1=%08lx\n", (unsigned long) PVR_GET(PVR_FB_ADDR), (unsigned long) PVR_GET(PVR_FB_IL_ADDR), (void*) vram_s, (unsigned long) PVR_GET(PVR_FB_CFG_1));
                const volatile uint16_t* fb = (const volatile uint16_t*) (PVR_RAM_BASE + off);
                static char line[64 * 4 + 16];
                for (int y = 0; y < 480; y++)
                    for (int k = 0; k < 10; k++) {
                        int n = sprintf(line, "FB%03d_%d:", y, k);
                        for (int x = k * 64; x < (k + 1) * 64; x++) n += sprintf(line + n, "%04x", fb[y * 640 + x]);
                        logInfo("%s\n", line);
                    }
            }
        }
        if (runner->shouldExit) {
            /* game_end() from the story (the fake crash before Omega Flowey, the end of the
               Flowey fight, the genocide ending...). On PC the program closes and the player
               starts it again; here we save, show a black screen for a moment and relaunch the
               game the same way game_restart() does, with the save files kept. */
            logInfo("DC: game_end() -> relaunching (frame %d)\n", runner->frameCount);
            audioSystem->vtable->stopAll(audioSystem);
            DcFileSystem_flush(fileSystem);
            dcCenterWindow();
            for (int i = 0; i < 45; i++) {
                DcRenderer_beginScene(renderer);
                DcRenderer_endScene(renderer);
            }
            DcFileSystem_waitIdle(fileSystem);
            runner->shouldExit = false;
            runner->pendingRoom = ROOM_RESTARTGAME;
        }
        {
            uint64_t rc0 = timer_us_gettime64();
            bool changing = runner->pendingRoom != -1;
            Runner_handlePendingRoomChange(runner);
            if (changing) { lagUs = 0; prevFrameStart = 0; }
            if (changing) logInfo("DC: room change took %u ms\n", (unsigned) ((timer_us_gettime64() - rc0) / 1000));
        }
        if (runner->currentRoom == nullptr) break;

        if (t2 - t0 > 40000 && frameCounter > 120)
            logInfo("DC: slow frame %llu: step %u ms, draw %u ms (room %s)\n", (unsigned long long) frameCounter, (unsigned) ((t1 - t0) / 1000), (unsigned) ((t2 - t1) / 1000), runner->currentRoom ? runner->currentRoom->name : "?");
        stats.stepMs = (float) (t1 - t0) / 1000.0f;
        stats.drawMs = (float) (t2 - t1) / 1000.0f;
        frameCounter++;
        fpsFrames++;
        uint64_t now = timer_us_gettime64();
        if (now - fpsWindowStart >= 1000000) {
            stats.fps = fpsFrames;
            if (fpsFrames < 29 && frameCounter > 120)
                logInfo("DC: fps dip %d (step %.1fms draw %.1fms) room %s\n", fpsFrames, (double) stats.stepMs, (double) stats.drawMs, runner->currentRoom ? runner->currentRoom->name : "?");
            fpsFrames = 0;
            fpsWindowStart = now;
        }
        if (now - lastLog >= 5000000) {
            const DcRendererStats* rs = DcRenderer_stats(renderer);
            struct mallinfo mi = mallinfo();
            const BytecodeCacheStats* bc = BytecodeCache_stats();
            logInfo("DC: bytecode cache %u KB, hits %u misses %u evictions %u\n", bc->cachedBytes / 1024, bc->hits, bc->misses, bc->evictions);
            logInfo("DC: frame %llu fps %d (catch-up %u) step %.1fms draw %.1fms polys %u pages %u (loads %u, evictions %u) vram %uKB heap %dKB free %uKB room %s\n",
                    (unsigned long long) frameCounter, stats.fps, (unsigned) nullFrames, (double) stats.stepMs, (double) stats.drawMs,
                    rs->polysFrame, rs->pagesLoaded, rs->pageLoadsTotal, rs->evictions, rs->vramFree / 1024, mi.uordblks / 1024, freeRamKB(),
                    runner->currentRoom ? runner->currentRoom->name : "?");
            {
                pvr_stats_t ps;
                if (pvr_get_stats(&ps) == 0 && ps.vtx_buffer_used_max > 0) logInfo("DC: TA vertex buffer max %u KB\n", (unsigned) (ps.vtx_buffer_used_max / 1024));
            }
            if (g_winMaxShift) { logInfo("DC: window shake up to %d px\n", (int) g_winMaxShift); g_winMaxShift = 0; }
            lastLog = now;
        }

        /* Frame pacing: hold each game frame for 60/room_speed vblanks (2 at 30 fps), so the
           display shows a perfectly even cadence. If a frame overruns we simply continue. */
        uint32_t vblPerFrame = roomSpeed >= 60 ? 1 : (60 + roomSpeed / 2) / roomSpeed;
        if (!nullFrame && lagUs < frameUs)
            while (g_vblanks - lastFlipVbl < vblPerFrame) thd_pass();   /* behind schedule: don't wait */
        lastFlipVbl = g_vblanks;
    }

    DcFileSystem_flush(fileSystem);
    DcFileSystem_waitIdle(fileSystem);
    audioSystem->vtable->stopAll(audioSystem);
    arch_menu();
    return 0;
}
