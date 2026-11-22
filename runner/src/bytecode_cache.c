/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "bytecode_cache.h"

#ifdef BS_COMPRESSED_BYTECODE

#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "utils.h"
#include "miniz.h"

typedef struct {
    bool active;
    uint32_t count;
    uint8_t** comp;
    uint32_t* compSize;
    uint32_t* rawSize;
    uint8_t** cached;
    uint16_t* pins;
    uint32_t* lastUse;
    uint32_t budget;
    uint32_t counter;
    BytecodeCacheStats stats;
} Cache;

static Cache g_cache;

static DataWin* g_sortDw;
static int cmpOffsetDesc(const void* a, const void* b) {
    uint32_t oa = g_sortDw->code.entries[*(const int32_t*) a].bytecodeAbsoluteOffset;
    uint32_t ob = g_sortDw->code.entries[*(const int32_t*) b].bytecodeAbsoluteOffset;
    return oa < ob ? 1 : (oa > ob ? -1 : 0);
}

bool BytecodeCache_compressAll(DataWin* dw, uint32_t cacheBudgetBytes, int level) {
    if (g_cache.active || dw->bytecodeBuffer == nullptr || dw->code.count == 0) return false;
    uint32_t n = dw->code.count;
    int32_t* order = (int32_t*) malloc(n * sizeof(int32_t));
    if (!order) return false;
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        CodeEntry* c = &dw->code.entries[i];
        if (c->present && c->length > 0) order[m++] = (int32_t) i;
    }
    g_sortDw = dw;
    qsort(order, m, sizeof(int32_t), cmpOffsetDesc);
    /* Entries must own disjoint, in-bounds regions (true for GMS 1.x bytecode; GMS 2.3+ child functions share them) */
    for (uint32_t k = 0; k < m; k++) {
        CodeEntry* c = &dw->code.entries[order[k]];
        if (c->bytecodeAbsoluteOffset < dw->bytecodeBufferBase) { free(order); return false; }
        if (k > 0) {
            CodeEntry* prev = &dw->code.entries[order[k - 1]];
            if (c->bytecodeAbsoluteOffset + c->length > prev->bytecodeAbsoluteOffset) {
                logInfo("BytecodeCache: overlapping code entries, compression disabled\n");
                free(order);
                return false;
            }
        }
    }

    g_cache.count = n;
    g_cache.comp = (uint8_t**) safeCalloc(n, sizeof(uint8_t*));
    g_cache.compSize = (uint32_t*) safeCalloc(n, sizeof(uint32_t));
    g_cache.rawSize = (uint32_t*) safeCalloc(n, sizeof(uint32_t));
    g_cache.cached = (uint8_t**) safeCalloc(n, sizeof(uint8_t*));
    g_cache.pins = (uint16_t*) safeCalloc(n, sizeof(uint16_t));
    g_cache.lastUse = (uint32_t*) safeCalloc(n, sizeof(uint32_t));
    g_cache.budget = cacheBudgetBytes;

    mz_uint flags = tdefl_create_comp_flags_from_zip_params(level, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
    uint8_t* buf = dw->bytecodeBuffer;
    size_t base = dw->bytecodeBufferBase;
    size_t keep = (size_t) -1;          /* bytes of the blob still needed */
    size_t lastShrink = (size_t) -1;
    for (uint32_t k = 0; k < m; k++) {
        int32_t idx = order[k];
        CodeEntry* c = &dw->code.entries[idx];
        size_t rel = c->bytecodeAbsoluteOffset - base;
        size_t outLen = 0;
        void* out = tdefl_compress_mem_to_heap(buf + rel, c->length, &outLen, (int) flags);
        if (!out) {
            logError("BytecodeCache: out of memory while compressing entry %d\n", idx);
            abort();
        }
        g_cache.comp[idx] = (uint8_t*) out;
        g_cache.compSize[idx] = (uint32_t) outLen;
        g_cache.rawSize[idx] = c->length;
        g_cache.stats.compressedBytes += (uint32_t) outLen;
        g_cache.stats.originalBytes += c->length;
        keep = rel;
        /* give the processed tail of the blob back to the heap every 128 KB */
        if (lastShrink == (size_t) -1 || lastShrink - keep >= 128 * 1024) {
            uint8_t* nb = (uint8_t*) realloc(buf, keep ? keep : 1);
            if (nb) buf = nb;
            lastShrink = keep;
        }
    }
    free(buf);
    free(order);
    dw->bytecodeBuffer = nullptr;
    g_cache.active = true;
    logInfo("BytecodeCache: %u entries, %u KB -> %u KB compressed, cache budget %u KB\n", m,
            g_cache.stats.originalBytes / 1024, g_cache.stats.compressedBytes / 1024, cacheBudgetBytes / 1024);
    return true;
}

bool BytecodeCache_isActive(void) { return g_cache.active; }

static bool evictOne(void) {
    int32_t victim = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < g_cache.count; i++) {
        if (!g_cache.cached[i] || g_cache.pins[i]) continue;
        if (g_cache.lastUse[i] < oldest) { oldest = g_cache.lastUse[i]; victim = (int32_t) i; }
    }
    if (victim < 0) return false;
    free(g_cache.cached[victim]);
    g_cache.cached[victim] = nullptr;
    g_cache.stats.cachedBytes -= g_cache.rawSize[victim];
    g_cache.stats.evictions++;
    return true;
}

const uint8_t* BytecodeCache_acquire(DataWin* dw, int32_t codeIndex) {
    if (!g_cache.active) {
        CodeEntry* c = &dw->code.entries[codeIndex];
        return dw->bytecodeBuffer + (c->bytecodeAbsoluteOffset - dw->bytecodeBufferBase);
    }
    static const uint8_t emptyCode[16] = {0};
    if (codeIndex >= 0 && (uint32_t) codeIndex < g_cache.count && !g_cache.comp[codeIndex] && dw->code.entries[codeIndex].length == 0)
        return emptyCode; /* empty event: nothing to execute */
    if (codeIndex < 0 || (uint32_t) codeIndex >= g_cache.count || !g_cache.comp[codeIndex]) {
        logError("BytecodeCache: no bytecode for code entry %d\n", codeIndex);
        abort();
    }
    g_cache.lastUse[codeIndex] = ++g_cache.counter;
    if (g_cache.cached[codeIndex]) {
        g_cache.pins[codeIndex]++;
        g_cache.stats.hits++;
        return g_cache.cached[codeIndex];
    }
    uint32_t raw = g_cache.rawSize[codeIndex];
    while (g_cache.stats.cachedBytes + raw > g_cache.budget && evictOne()) {}
    uint8_t* mem = (uint8_t*) malloc(raw + 4);
    while (!mem && evictOne()) mem = (uint8_t*) malloc(raw + 4);
    if (!mem) {
        logError("BytecodeCache: out of memory for entry %d (%u bytes)\n", codeIndex, raw);
        abort();
    }
    size_t got = tinfl_decompress_mem_to_mem(mem, raw, g_cache.comp[codeIndex], g_cache.compSize[codeIndex], 0);
    if (got != raw) {
        logError("BytecodeCache: corrupt entry %d\n", codeIndex);
        abort();
    }
    g_cache.cached[codeIndex] = mem;
    g_cache.pins[codeIndex] = 1;
    g_cache.stats.cachedBytes += raw;
    g_cache.stats.misses++;
    return mem;
}

void BytecodeCache_release(int32_t codeIndex) {
    if (!g_cache.active) return;
    if (codeIndex >= 0 && (uint32_t) codeIndex < g_cache.count && g_cache.pins[codeIndex] > 0) g_cache.pins[codeIndex]--;
}

const BytecodeCacheStats* BytecodeCache_stats(void) { return &g_cache.stats; }

#else

bool BytecodeCache_compressAll(MAYBE_UNUSED DataWin* dw, MAYBE_UNUSED uint32_t budget, MAYBE_UNUSED int level) { return false; }
bool BytecodeCache_isActive(void) { return false; }
const uint8_t* BytecodeCache_acquire(DataWin* dw, int32_t codeIndex) {
    CodeEntry* c = &dw->code.entries[codeIndex];
    return dw->bytecodeBuffer + (c->bytecodeAbsoluteOffset - dw->bytecodeBufferBase);
}
void BytecodeCache_release(MAYBE_UNUSED int32_t codeIndex) {}
static BytecodeCacheStats g_noStats;
const BytecodeCacheStats* BytecodeCache_stats(void) { return &g_noStats; }

#endif
