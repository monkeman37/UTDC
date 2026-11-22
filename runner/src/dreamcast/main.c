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
#include <dc/vblank.h>

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_MALLOCSTATS);

#define DISC_ROOT "/cd"
#define STICK_DEADZONE 48

/* ===[ logging ]=== */
static bool g_quietLogs = false;
void platformLog(const logType type, const char* format, va_list va) {
    if (g_quietLogs && type == LOG_TYPE_DEBUG) return;
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
static void loadProgress(const char* chunkName, int chunkIndex, int totalChunks, MAYBE_UNUSED DataWin* dw, MAYBE_UNUSED void* user) {
    char line[64];
    snprintf(line, sizeof line, "Reading game data (%s)", chunkName);
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

    /* optional USB/Dreamcast keyboard */
    maple_device_t* kbd = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    if (kbd) {
        int k;
        while ((k = kbd_queue_pop(kbd, 1)) != KBD_QUEUE_END) {
            (void) k; /* keyboard support is best-effort; controller is primary */
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

int main(MAYBE_UNUSED int argc, MAYBE_UNUSED char** argv) {
    vid_set_mode(DM_640x480, PM_RGB565);
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
            char buf[512];
            ssize_t n = fs_read(f, buf, sizeof buf - 1);
            fs_close(f);
            if (n > 0) {
                buf[n] = 0;
                char* line = strtok(buf, "\r\n");
                while (line) {
                    if (!strncmp(line, "title=", 6)) snprintf(title, sizeof title, "%s", line + 6);
                    else if (!strncmp(line, "overlay=", 8)) showOverlay = atoi(line + 8) != 0;
                    else if (!strncmp(line, "exitframe=", 10)) exitFrame = atoi(line + 10);
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
    options.loadType = DATAWINLOADTYPE_LOAD_PER_CHUNK;
    options.progressCallback = loadProgress;

    DataWin* dataWin = DataWin_parse(DISC_ROOT "/DATA.WIN", options);
    if (!dataWin) fatal("Could not read DATA.WIN from the disc.", "Rebuild the image with the UTDC wizard.");
    DcTextPatches_apply(dataWin);
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
    params.vertex_buf_size = 640 * 1024;
    params.dma_enabled = 0;
    params.fsaa_enabled = 0;
    params.autosort_disabled = 1;
    params.opb_overflow_count = 2;
    if (pvr_init(&params) < 0) fatal("Could not initialise the PowerVR.", nullptr);
    pvr_set_bg_color(0, 0, 0);

    Renderer* renderer = DcRenderer_create(DISC_ROOT "/TEXTURES.BIN");
    if (!renderer) fatal("Could not read TEXTURES.BIN from the disc.", nullptr);

    FileSystem* fileSystem = DcFileSystem_create(dataWin->gen8.displayName, DISC_ROOT);

    snd_init();
    AudioSystem* audioSystem = DcAudioSystem_create(DISC_ROOT "/AUDIO.BIN");
    if (!audioSystem) fatal("Could not read AUDIO.BIN from the disc.", nullptr);

    VMContext* vm = VM_create(dataWin);
    textScreen(title, "Packing game code...", nullptr, 1.0f);
    /* Deflate the patched bytecode per script and keep a 1 MB cache of decompressed scripts:
       saves roughly 3-4 MB of the Dreamcast's 16 MB. */
    BytecodeCache_compressAll(dataWin, 1024 * 1024, 3);
    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem, 0);
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

    vblank_handler_add(onVblank, nullptr);
    Runner_initFirstRoom(runner);

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

    while (!runner->shouldExit) {
        uint64_t frameStart = timer_us_gettime64();
        runner->deltaTime = (GMLReal) (frameStart - lastFrame);
        lastFrame = frameStart;

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

        DcRenderer_beginScene(renderer);
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
        uint64_t t2 = timer_us_gettime64();

        RunnerKeyboard_beginFrame(runner->keyboard);

        uint32_t roomSpeed = runner->currentRoom ? runner->currentRoom->speed : 30;
        if (roomSpeed == 0) roomSpeed = 30;
        audioSystem->vtable->update(audioSystem, 1.0f / (float) roomSpeed);
        DcFileSystem_tick(fileSystem);

        Runner_handlePendingRoomChange(runner);

        stats.stepMs = (float) (t1 - t0) / 1000.0f;
        stats.drawMs = (float) (t2 - t1) / 1000.0f;
        frameCounter++;
        fpsFrames++;
        uint64_t now = timer_us_gettime64();
        if (now - fpsWindowStart >= 1000000) {
            stats.fps = fpsFrames;
            fpsFrames = 0;
            fpsWindowStart = now;
        }
        if (now - lastLog >= 5000000) {
            const DcRendererStats* rs = DcRenderer_stats(renderer);
            struct mallinfo mi = mallinfo();
            const BytecodeCacheStats* bc = BytecodeCache_stats();
            logInfo("DC: bytecode cache %u KB, hits %u misses %u evictions %u\n", bc->cachedBytes / 1024, bc->hits, bc->misses, bc->evictions);
            logInfo("DC: frame %llu fps %d step %.1fms draw %.1fms polys %u pages %u (loads %u, evictions %u) vram %uKB heap %dKB free %uKB room %s\n",
                    (unsigned long long) frameCounter, stats.fps, (double) stats.stepMs, (double) stats.drawMs,
                    rs->polysFrame, rs->pagesLoaded, rs->pageLoadsTotal, rs->evictions, rs->vramFree / 1024, mi.uordblks / 1024, freeRamKB(),
                    runner->currentRoom ? runner->currentRoom->name : "?");
            lastLog = now;
        }

        /* Frame pacing: hold each game frame for 60/room_speed vblanks (2 at 30 fps), so the
           display shows a perfectly even cadence. If a frame overruns we simply continue. */
        uint32_t vblPerFrame = roomSpeed >= 60 ? 1 : (60 + roomSpeed / 2) / roomSpeed;
        while (g_vblanks - lastFlipVbl < vblPerFrame) thd_pass();
        lastFlipVbl = g_vblanks;
    }

    DcFileSystem_flush(fileSystem);
    DcFileSystem_waitIdle(fileSystem);
    audioSystem->vtable->stopAll(audioSystem);
    arch_menu();
    return 0;
}
