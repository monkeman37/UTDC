/* Replaces keyboard prompts in the game's string table with Dreamcast button names.
 *
 * Runs once after data.win is loaded and before any game code executes. Only
 * string *constants* are touched (no scripts change), and only exact,
 * hand-picked matches, so a mismatch simply leaves the original text.
 *
 * Button layout (see main.c): A = Confirm, B = Cancel, Y = Menu,
 * Start = Confirm/skip, A+B+X+Y+Start = return to the Dreamcast menu.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <string.h>
#include <stdlib.h>
#include "dc_text_patches.h"
#include "utils.h"
#include "log.h"

typedef struct {
    const char* from;
    const char* to;
    bool substring;   /* replace inside a longer string instead of the whole string */
} TextPatch;

static const TextPatch PATCHES[] = {
    /* title screen */
    { "[PRESS Z OR ENTER]", "[PRESS START]", false },
    /* instruction screen (English) */
    { "[Z or ENTER]", "[A or START]", false },
    { "[X or SHIFT]", "[B]", false },
    { "[C or CTRL]", "[Y]", false },
    { "[F4]", "[--]", false },
    { "[Hold ESC]", "[ABXY+START]", false },
    /* instruction screen (Japanese) */
    { "[Z / ENTER]", "[A / START]", false },
    { "[X / SHIFT]", "[B]", false },
    { "[C / CTRL]", "[Y]", false },
    { "[ESC\xE3\x81\xAA\xE3\x81\x8C\xE3\x81\x8A\xE3\x81\x97]", "[ABXY+START]", false },
    /* in-game tutorial line that names the keyboard key for the menu */
    { "THE [ C ] KEY ON&YOUR KEYBOARD FOR", "THE [ Y ] BUTTON&ON YOUR PAD FOR", true },
};

/* Stores 'text' as entry i: in place if it fits (keeps the GameMaker length prefix
   right), otherwise in a new length-prefixed allocation. */
static void setString(DataWin* dw, uint32_t i, const char* text) {
    size_t newLen = strlen(text);
    char* cur = (char*) dw->strg.strings[i];
    size_t curLen = strlen(cur);
    if (newLen <= curLen && BS_isStaticString(cur)) {
        memcpy(cur, text, newLen + 1);
        uint32_t len32 = (uint32_t) newLen;
        memcpy(cur - 4, &len32, 4);
        return;
    }
    char* block = (char*) safeMalloc(4 + newLen + 1);
    uint32_t len32 = (uint32_t) newLen;
    memcpy(block, &len32, 4);
    memcpy(block + 4, text, newLen + 1);
    dw->strg.strings[i] = block + 4;
}

int DcTextPatches_apply(DataWin* dw) {
    int applied = 0;
    for (uint32_t i = 0; i < dw->strg.count; i++) {
        const char* s = dw->strg.strings[i];
        if (s == nullptr || s[0] == 0) continue;
        for (size_t p = 0; p < sizeof PATCHES / sizeof PATCHES[0]; p++) {
            const TextPatch* tp = &PATCHES[p];
            if (!tp->substring) {
                if (strcmp(s, tp->from) == 0) { setString(dw, i, tp->to); applied++; break; }
                continue;
            }
            const char* at = strstr(s, tp->from);
            if (!at) continue;
            size_t fromLen = strlen(tp->from), toLen = strlen(tp->to), sLen = strlen(s);
            char* out = (char*) safeMalloc(sLen - fromLen + toLen + 1);
            size_t pre = (size_t) (at - s);
            memcpy(out, s, pre);
            memcpy(out + pre, tp->to, toLen);
            strcpy(out + pre + toLen, at + fromLen);
            setString(dw, i, out);
            free(out);
            applied++;
            break;
        }
    }
    logInfo("DC: %d on-screen key prompts changed to Dreamcast buttons\n", applied);
    return applied;
}
