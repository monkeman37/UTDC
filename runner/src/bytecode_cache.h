/* Compressed bytecode store with an on-demand decompression cache.
 *
 * On memory-starved targets (Sega Dreamcast: 16 MB RAM) the patched GML
 * bytecode blob is by far the biggest allocation. After VM_create() has
 * patched it, BytecodeCache_compressAll() deflates every code entry
 * separately and frees the blob. VM_executeCode() then acquires a
 * decompressed copy of the entry it runs; entries in use by the call stack
 * are pinned, the rest are evicted least-recently-used.
 *
 * Only compiled in when BS_COMPRESSED_BYTECODE is defined.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef _BS_BYTECODE_CACHE_H_
#define _BS_BYTECODE_CACHE_H_

#include "common.h"
#include <stdint.h>
#include <stdbool.h>
#include "data_win.h"

typedef struct {
    uint32_t compressedBytes;
    uint32_t originalBytes;
    uint32_t cachedBytes;
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
} BytecodeCacheStats;

/* Returns false (and leaves the raw blob untouched) if the layout isn't supported. */
bool BytecodeCache_compressAll(DataWin* dw, uint32_t cacheBudgetBytes, int level);
bool BytecodeCache_isActive(void);
const uint8_t* BytecodeCache_acquire(DataWin* dw, int32_t codeIndex);
void BytecodeCache_release(int32_t codeIndex);
const BytecodeCacheStats* BytecodeCache_stats(void);

#endif
