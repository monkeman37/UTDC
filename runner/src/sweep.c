// Automated coverage sweep used by the UTDC test harness (CLI builds only, enabled by env vars).
//   BS_SWEEP_MODE   = rooms | battles | objects (instance_create the listed objects) | sounds (play each sound once)
//   BS_SWEEP_LIST   = comma separated room indices / battle groups
//   BS_SWEEP_START  = index into the list to start from (default 0)
//   BS_SWEEP_BOOT   = frame at which the sweep starts (default 400)
//   BS_SWEEP_FRAMES = frames spent per item (default 300)
// Every frame the sweep mashes CONFIRM/CANCEL/arrows pseudo-randomly so menus, dialogue and
// battle turns advance. The player's HP is refilled so battles keep going.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "runner.h"
#include "runner_keyboard.h"
#include "instance.h"
#include "vm.h"
#include "log.h"
#include "stb_ds.h"
#include "sweep.h"

static int g_mode = -1; // 0 rooms, 1 battles
static int* g_list;
static int g_count, g_pos, g_boot = 400, g_frames = 300, g_next;
static uint32_t g_rng = 12345;
static int32_t g_soundId = -1, g_itemStart = 0;

static uint32_t rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 16; }

static void setGlobal(Runner* runner, const char* name, RValue v) {
    int32_t id = shget(runner->vmContext->varNameMap, (char*) name);
    if (id >= 0) Instance_setSelfVar(runner->vmContext->globalScopeInstance, id, v);
}
static RValue getGlobal(Runner* runner, const char* name) {
    int32_t id = shget(runner->vmContext->varNameMap, (char*) name);
    return id >= 0 ? Instance_getSelfVar(runner->vmContext->globalScopeInstance, id) : RValue_makeUndefined();
}

static void init(void) {
    const char* m = getenv("BS_SWEEP_MODE");
    if (m == nullptr) { g_mode = -2; return; }
    g_mode = strcmp(m, "battles") == 0 ? 1 : strcmp(m, "objects") == 0 ? 2 : strcmp(m, "sounds") == 0 ? 3 : 0;
    const char* l = getenv("BS_SWEEP_LIST");
    for (const char* p = l ? l : ""; *p;) {
        char* end; long v = strtol(p, &end, 10);
        if (end == p) { p++; continue; }
        arrput(g_list, (int) v); p = end;
    }
    g_count = (int) arrlen(g_list);
    if (getenv("BS_SWEEP_START")) g_pos = atoi(getenv("BS_SWEEP_START"));
    if (getenv("BS_SWEEP_BOOT")) g_boot = atoi(getenv("BS_SWEEP_BOOT"));
    if (getenv("BS_SWEEP_FRAMES")) g_frames = atoi(getenv("BS_SWEEP_FRAMES"));
    g_next = g_boot;
}

void Sweep_tick(Runner* runner) {
    if (g_mode == -1) init();
    if (g_mode < 0) return;
    int frame = runner->frameCount;
    if (frame < g_boot) return;
    if (frame >= g_next) {
        if (g_pos >= g_count) {
            if (g_pos == g_count) { logInfo("SWEEP: done\n"); g_pos++; }
#ifndef PLATFORM_DREAMCAST
            runner->shouldExit = true;
#endif
            return;
        }
        int item = g_list[g_pos++];
        g_next = frame + g_frames;
        if (g_mode == 0) {
            if (item >= 0 && (uint32_t) item < runner->dataWin->room.count) {
                logInfo("SWEEP: item %d room %d %s (frame %d)\n", g_pos - 1, item, runner->dataWin->room.rooms[item].name, frame);
                setGlobal(runner, "interact", RValue_makeInt32(0));
                runner->pendingRoom = item;
            }
        } else if (g_mode == 3) {
            runner->audioSystem->vtable->stopAll(runner->audioSystem);
            int32_t id = runner->audioSystem->vtable->playSound(runner->audioSystem, item, 50, false);
            g_soundId = id; g_itemStart = frame;
            logInfo("SWEEP: item %d sound %d %s -> instance %d (frame %d)\n", g_pos - 1, item,
                    (uint32_t) item < runner->dataWin->sond.count ? runner->dataWin->sond.sounds[item].name : "?", (int) id, frame);
        } else if (g_mode == 2) {
            if (item >= 0 && (uint32_t) item < runner->dataWin->objt.count) {
                logInfo("SWEEP: item %d object %d %s (frame %d)\n", g_pos - 1, item, runner->dataWin->objt.objects[item].name, frame);
                Runner_createInstance(runner, 0, 0, item);
            }
        } else {
            logInfo("SWEEP: item %d battlegroup %d (frame %d)\n", g_pos - 1, item, frame);
            setGlobal(runner, "battlegroup", RValue_makeInt32(item));
            setGlobal(runner, "interact", RValue_makeInt32(0));
            int32_t battleRoom = -1;
            repeat(runner->dataWin->room.count, i) if (strcmp(runner->dataWin->room.rooms[i].name, "room_battle") == 0) battleRoom = (int32_t) i;
            if (battleRoom >= 0) runner->pendingRoom = battleRoom;
        }
    }
    // BS_SWEEP_PAUSETEST: pause each played sound 3 s in, resume it 2 s later (music pause/resume check)
    if (g_mode == 3 && g_soundId >= 0 && getenv("BS_SWEEP_PAUSETEST")) {
        if (frame == g_itemStart + 90) { runner->audioSystem->vtable->pauseSound(runner->audioSystem, g_soundId); logInfo("SWEEP: paused %d\n", g_soundId); }
        if (frame == g_itemStart + 150) { runner->audioSystem->vtable->resumeSound(runner->audioSystem, g_soundId); logInfo("SWEEP: resumed %d\n", g_soundId); }
    }
    // keep the player alive in battles
    if (g_mode == 1) {
        RValue mx = getGlobal(runner, "maxhp");
        if (mx.type == RVALUE_REAL || mx.type == RVALUE_INT32) setGlobal(runner, "hp", mx);
    }
    if (getenv("BS_SWEEP_NOINPUT")) return;
    // mash inputs
    static const int32_t keys[] = { 'Z', 'Z', 'Z', 'X', VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_ENTER };
    static int32_t held = -1;
    if (held >= 0) { RunnerKeyboard_simulateRelease(runner->keyboard, held); held = -1; }
    if ((frame & 3) == 0) {
        held = keys[rnd() % (sizeof keys / sizeof keys[0])];
        RunnerKeyboard_simulatePress(runner->keyboard, held);
    }
}
