/* PowerVR2 renderer for Butterscotch on the Sega Dreamcast.
 *
 * Design
 *  - Everything is drawn in the translucent list with auto-sort disabled, so
 *    polygons are composited in submission order like GameMaker expects.
 *  - Vertices are sent straight to the Tile Accelerator with pvr_prim() while
 *    the game's Draw events run (no RAM-side vertex buffer).
 *  - Textures come from TEXTURES.BIN (built by the PC-side builder): small
 *    pre-twiddled 16bpp pages that are streamed from disc into VRAM on first
 *    use and evicted least-recently-used when VRAM runs out.
 *  - Texture uploads are plain CPU stores into VRAM so they never fight the
 *    store queues that feed the TA.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <kos.h>
#include <dc/pvr.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "dc_renderer.h"
#include "../../../common/utdc_formats.h"
#include "text_utils.h"
#include "utils.h"
#include "log.h"
#include "default_font/default_font.h"
#include "data_win.h"
#include "stb_ds.h"


#define SCREEN_W 640.0f
#define SCREEN_H 480.0f
#define MAX_PRIM_VERTS 4096
/* Each quad/triangle uses <= 160 bytes of the 384 KB TA vertex buffer; stay well inside it */
#define DC_MAX_POLYS 2200
#define DC_LZ4_IN_MAX (UTDC_TEX_BLOCK_RAW + UTDC_TEX_BLOCK_RAW / 255 + 64)

typedef struct {
    pvr_ptr_t vram;
    uint32_t lastUsed;
    uint32_t bytes;
    uint8_t* packed;        /* compressed payload kept in RAM after eviction (see PACKED_CACHE_BUDGET) */
    uint32_t packedUse;     /* LRU stamp for the packed cache */
    uint32_t retryFrame;    /* after a failed load, don't retry before this frame */
} DcPageSlot;

/* Compressed pages are small (typically 5-80 KB). Keeping recently used ones in RAM means that
   going back and forth between a room and a battle doesn't hit the GD-ROM (seek time on real
   hardware is far worse than the emulator suggests). */
#define PACKED_CACHE_BUDGET (320 * 1024)

typedef struct {
    Renderer base;

    /* texture data */
    file_t texFile;
    UtdcTexHeader texHdr;
    UtdcTexPage* pages;
    UtdcTexItem* items;
    DcPageSlot* slots;
    uint32_t frame;
    uint8_t* dict;           /* decoded LZ4 block (32 KB) */
    pvr_ptr_t whiteTex;      /* 8x8 white texture for untextured polys */

    /* transform: screen = world * scale + offset */
    float scaleX, scaleY, offX, offY;
    float viewAngleCos, viewAngleSin, viewCX, viewCY;
    bool viewRotated;
    int32_t gameW, gameH;
    float winScaleX, winScaleY;

    /* state */
    int32_t curPage;         /* -2 = none emitted yet, -1 = untextured */
    int32_t curSrc, curDst;
    bool curFilter;
    bool blendEnabled;
    int32_t blendSrc, blendDst;   /* GM bm_* factors */
    bool inScene;
    bool nullFrame;     /* catch-up frame: draw events run, nothing is rendered or uploaded */
    bool drawingToSurface;

    /* primitive batching */
    int32_t primType;
    int32_t primTexture;     /* tpag index or -1 */
    int32_t primCount;
    float primX[MAX_PRIM_VERTS], primY[MAX_PRIM_VERTS], primU[MAX_PRIM_VERTS], primV[MAX_PRIM_VERTS];
    uint32_t primC[MAX_PRIM_VERTS];

    /* surfaces (only bookkeeping, see header comment) */
    int32_t surfaceCount;
    int32_t* surfaceW;
    int32_t* surfaceH;
    bool* surfaceAlive;
    int32_t currentTarget;

    /* screen snapshots (sprite_create_from_surface on application_surface) */
    uint32_t origTpagCount, origSpriteCount;

    DcRendererStats stats;
} DcRenderer;

/* A snapshot is a copy of the last displayed frame (or part of it) in a non-twiddled
   ARGB1555 texture. The framebuffer is the game picture at 2x, so every other pixel is taken
   and the texture holds the rect at half resolution. Snapshot "pages" use ids above the
   TEXTURES.BIN page count. */
typedef struct {
    pvr_ptr_t vram;
    uint16_t texW, texH;     /* power-of-two texture size */
    float scale;             /* sprite pixels per texel (2) */
    uint32_t freeAtFrame;    /* 0 = alive */
    bool used;
    /* render-to-texture captures (emulators that can't read the framebuffer back, see below) */
    bool rtt;                /* RGB565 texture written by the PVR */
    bool pending;            /* not rendered yet (filled at the end of the frame it was made in) */
    bool live;               /* currently points into the rolling capture */
    bool ownsVram;           /* vram is this snapshot's own (freed with it) */
    float offU, offV;        /* texel offset of the rect inside the texture */
    int16_t rectX, rectY;    /* captured rect, window pixels */
    uint16_t rectW, rectH;
} DcSnapshot;
static DcSnapshot* g_snaps;   /* stb array */
static int32_t* g_tpagSnap;   /* stb array, index = tpag - origTpagCount */

/* ---------------------------------------------------------------------
 * Screen captures without framebuffer read-back
 *
 * Undertale copies the screen for some effects (the True Lab elevator losing power, Flowey's
 * defeated soul, Alphys's lab monitor, heat shimmer, screen waves). On a real Dreamcast the
 * rendered picture is in VRAM and is read back directly. Emulators render on the host GPU and,
 * with default settings, never write the picture back, so reading VRAM returned stale data (the
 * boot loading screen). A boot-time probe detects that case; then:
 *  - every frame's TA stream is recorded (double buffered);
 *  - a capture made during frame N gets its own texture, rendered with render-to-texture from
 *    the recording of frame N-1 right after frame N (it shows from frame N+1 on);
 *  - while captures are being made every frame, a half-size "rolling" capture of each frame is
 *    rendered too, and new captures point into it at once (no one-frame gap for per-frame
 *    effects). Captures that outlive their frame are then given their own copy.
 * ------------------------------------------------------------------- */
static int g_fbReadable = -1;      /* -1 not probed, 0 no (render-to-texture captures), 1 yes */
#define REC_CAP (192 * 1024)
static uint8_t* g_rec[2];          /* recorded 32-byte TA commands of the last two frames */
static uint8_t* g_recKind[2];      /* per command: 0 vertex, 1 header, 2 header sampling the rolling capture */
static uint32_t g_recLen[2];       /* commands */
static int g_recCur;
static bool g_replaying;
static pvr_ptr_t g_rollTex;        /* 512x256 RGB565, holds the previous frame at half size */
static uint32_t g_rollValidFrame;  /* frame whose picture the rolling capture holds */
static uint32_t g_lastSnapReq;     /* frame of the last capture request */
#define ROLL_W 512
#define ROLL_H 256

static void recPrim(const void* data, uint8_t kind) {
    pvr_prim((void*) data, 32);
    if (g_fbReadable != 0 || g_replaying || !g_rec[g_recCur]) return;
    uint32_t n = g_recLen[g_recCur];
    if ((n + 1) * 32 > REC_CAP) return;
    memcpy(g_rec[g_recCur] + n * 32, data, 32);
    g_recKind[g_recCur][n] = kind;
    g_recLen[g_recCur] = n + 1;
}

/* ===================================================================== */
/* Texture cache                                                          */
/* ===================================================================== */

static bool openTextures(DcRenderer* dc, const char* path) {
    dc->texFile = fs_open(path, O_RDONLY);
    if (dc->texFile == FILEHND_INVALID) { logError("DC: cannot open %s\n", path); return false; }
    if (fs_read(dc->texFile, &dc->texHdr, sizeof dc->texHdr) != (ssize_t) sizeof dc->texHdr ||
        dc->texHdr.magic != UTDC_TEX_MAGIC || dc->texHdr.version != UTDC_FORMAT_VERSION) {
        logError("DC: %s is not a valid TEXTURES.BIN (rebuild the disc with the matching builder)\n", path);
        return false;
    }
    size_t pagesBytes = dc->texHdr.pageCount * sizeof(UtdcTexPage);
    size_t itemsBytes = dc->texHdr.itemCount * sizeof(UtdcTexItem);
    dc->pages = (UtdcTexPage*) safeMalloc(pagesBytes);
    dc->items = (UtdcTexItem*) safeMalloc(itemsBytes);
    dc->slots = (DcPageSlot*) safeCalloc(dc->texHdr.pageCount, sizeof(DcPageSlot));
    if (fs_read(dc->texFile, dc->pages, pagesBytes) != (ssize_t) pagesBytes) return false;
    if (fs_read(dc->texFile, dc->items, itemsBytes) != (ssize_t) itemsBytes) return false;
    dc->dict = (uint8_t*) memalign(32, UTDC_TEX_BLOCK_RAW);
    logInfo("DC: TEXTURES.BIN: %u pages, %u items\n", dc->texHdr.pageCount, dc->texHdr.itemCount);
    return true;
}

static bool g_renderSynced; /* waited for the previous frame's render during this frame */

/* ---------------------------------------------------------------------
 * Texture VRAM pool
 *
 * Texture pages live in one big VRAM region managed here instead of in the KOS VRAM heap.
 * Pages range from 6 KB (small VQ pages) to 2 MB, and with the KOS heap a big page could fail to
 * fit even with plenty of VRAM free in total (fragmentation): every failed pvr_mem_malloc printed
 * an error over the serial port and only one page was evicted per retry, which stalled a frame
 * for over half a second in Omega Flowey's fight. Here a failed allocation picks the cheapest run
 * of neighbouring pages that frees a big enough contiguous hole and evicts exactly those, once.
 * ------------------------------------------------------------------- */
typedef struct { uint32_t off, size; int32_t page; } PoolBlock;   /* page -1: free */
static PoolBlock* g_pool;          /* stb array, sorted by offset, covers the whole region */
static uint8_t* g_poolBase;
static uint32_t g_poolSize, g_poolFree;
#define POOL_RESERVE (384 * 1024)  /* left in the KOS heap for screen snapshots and small buffers */

static void poolInit(void) {
    if (g_poolBase) return;
    size_t avail = pvr_mem_available();
    if (avail <= POOL_RESERVE + 65536) return;
    uint32_t size = (uint32_t) ((avail - POOL_RESERVE) & ~4095u);
    g_poolBase = (uint8_t*) pvr_mem_malloc(size);
    if (!g_poolBase) return;
    g_poolSize = g_poolFree = size;
    PoolBlock all = { 0, size, -1 };
    arrput(g_pool, all);
    logInfo("DC: texture pool %u KB\n", (unsigned) (size / 1024));
}

static void poolFreeIndex(int32_t i) {
    g_poolFree += g_pool[i].size;
    g_pool[i].page = -1;
    if (i + 1 < (int32_t) arrlen(g_pool) && g_pool[i + 1].page == -1) { g_pool[i].size += g_pool[i + 1].size; arrdel(g_pool, i + 1); }
    if (i > 0 && g_pool[i - 1].page == -1) { g_pool[i - 1].size += g_pool[i].size; arrdel(g_pool, i); }
}

static void poolFreePtr(pvr_ptr_t p) {
    uint32_t off = (uint32_t) ((uint8_t*) p - g_poolBase);
    for (int32_t i = 0; i < (int32_t) arrlen(g_pool); i++)
        if (g_pool[i].off == off && g_pool[i].page != -1) { poolFreeIndex(i); return; }
}

/* Small pages are placed best-fit from the bottom of the pool, big ones (256 KB and up, rarely
   used backgrounds and 1024-wide pages) at the top end, so the small pages a frame is drawing
   don't end up scattered through the space a big page needs. */
#define POOL_BIG (256 * 1024)
static pvr_ptr_t poolTryAlloc(uint32_t bytes, int32_t pageId) {
    bytes = (bytes + 31) & ~31u;
    bool big = bytes >= POOL_BIG;
    int32_t best = -1;
    for (int32_t i = 0; i < (int32_t) arrlen(g_pool); i++) {
        if (g_pool[i].page != -1 || g_pool[i].size < bytes) continue;
        if (big) best = i;   /* highest fitting block */
        else if (best < 0 || g_pool[i].size < g_pool[best].size) best = i;
    }
    if (best < 0) return nullptr;
    int32_t at = best;
    if (g_pool[best].size > bytes) {
        if (big) {
            /* carve from the top end of the block */
            PoolBlock top = { g_pool[best].off + g_pool[best].size - bytes, bytes, -1 };
            g_pool[best].size -= bytes;
            arrins(g_pool, best + 1, top);
            at = best + 1;
        } else {
            PoolBlock rest = { g_pool[best].off + bytes, g_pool[best].size - bytes, -1 };
            arrins(g_pool, best + 1, rest);
            g_pool[best].size = bytes;
        }
    }
    g_pool[at].page = pageId;
    g_poolFree -= bytes;
    return (pvr_ptr_t) (g_poolBase + g_pool[at].off);
}

/* Allocates VRAM for a page, evicting the cheapest run of neighbouring pages if needed.
   keepFrom: pages used at or after this frame are pinned (still drawn / still being read). */
static pvr_ptr_t poolAllocEvicting(DcRenderer* dc, uint32_t bytes, int32_t pageId) {
    pvr_ptr_t p = poolTryAlloc(bytes, pageId);
    if (p) return p;
    uint32_t need = (bytes + 31) & ~31u;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && !g_renderSynced) { pvr_wait_render_done(); g_renderSynced = true; }
        uint32_t keepFrom = pass == 0 ? dc->frame - 1 : dc->frame;
        int32_t n = (int32_t) arrlen(g_pool);
        int32_t bestI = -1, bestJ = -1;
        uint64_t bestCost = ~0ull;
        for (int32_t i = 0; i < n; i++) {
            uint32_t got = 0; uint64_t cost = 0;
            for (int32_t j = i; j < n; j++) {
                PoolBlock* b = &g_pool[j];
                if (b->page == -2) break;                             /* reserved (rolling capture) */
                if (b->page >= 0) {
                    uint32_t lu = dc->slots[b->page].lastUsed;
                    if (lu >= keepFrom) break;                       /* pinned: window can't span it */
                    /* cost: bytes thrown away, weighted up for recently used pages */
                    uint32_t age = dc->frame - lu;
                    cost += (uint64_t) b->size * (age < 30 ? 8 : age < 300 ? 2 : 1);
                }
                got += b->size;
                if (got >= need) {
                    bool bigAlloc = need >= POOL_BIG;
                    if (cost < bestCost || (bigAlloc && cost == bestCost)) { bestCost = cost; bestI = i; bestJ = j; }
                    break;
                }
            }
        }
        if (bestI >= 0) {
            /* evict the pages in [bestI, bestJ]; indices shift as blocks merge, so go by offset */
            uint32_t from = g_pool[bestI].off, to = g_pool[bestJ].off;
            for (int32_t k = 0; k < (int32_t) arrlen(g_pool); k++) {
                if (g_pool[k].off < from) continue;
                if (g_pool[k].off > to) break;
                if (g_pool[k].page >= 0) {
                    int32_t victim = g_pool[k].page;
                    dc->slots[victim].vram = nullptr;
                    dc->stats.pagesLoaded--;
                    dc->stats.evictions++;
                    poolFreeIndex(k);
                    k = -1;   /* restart: the array changed */
                }
            }
            p = poolTryAlloc(bytes, pageId);
            if (p) return p;
        }
    }
    return nullptr;
}

/* Minimal LZ4 block decoder (format: https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md).
   Returns decoded size or -1 on malformed input. */
static int lz4DecodeBlock(const uint8_t* src, int srcLen, uint8_t* dst, int dstCap) {
    const uint8_t* ip = src;
    const uint8_t* const iend = src + srcLen;
    uint8_t* op = dst;
    uint8_t* const oend = dst + dstCap;
    while (ip < iend) {
        unsigned token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) { unsigned b; do { if (ip >= iend) return -1; b = *ip++; lit += b; } while (b == 255); }
        if ((size_t) (iend - ip) < lit || (size_t) (oend - op) < lit) return -1;
        memcpy(op, ip, lit);
        op += lit; ip += lit;
        if (ip >= iend) break; /* last sequence has no match */
        if (iend - ip < 2) return -1;
        size_t off = (size_t) ip[0] | ((size_t) ip[1] << 8);
        ip += 2;
        if (off == 0 || off > (size_t) (op - dst)) return -1;
        size_t ml = token & 15;
        if (ml == 15) { unsigned b; do { if (ip >= iend) return -1; b = *ip++; ml += b; } while (b == 255); }
        ml += 4;
        if ((size_t) (oend - op) < ml) return -1;
        const uint8_t* m = op - off;
        if (off >= 8 && ml <= off) { memcpy(op, m, ml); op += ml; }
        else while (ml--) *op++ = *m++;
    }
    return (int) (op - dst);
}

/* Streams one page from disc into VRAM: each LZ4 block is decoded into a 32 KB RAM
   buffer and copied to VRAM with 32-bit stores (no store queues: they feed the TA). */
static uint32_t g_packedBytes, g_packedClock;

static void packedCacheTrim(DcRenderer* dc, uint32_t need) {
    while (g_packedBytes + need > PACKED_CACHE_BUDGET) {
        int32_t victim = -1; uint32_t oldest = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < dc->texHdr.pageCount; i++) {
            DcPageSlot* s = &dc->slots[i];
            if (s->packed && s->packedUse < oldest) { oldest = s->packedUse; victim = (int32_t) i; }
        }
        if (victim < 0) return;
        free(dc->slots[victim].packed);
        dc->slots[victim].packed = nullptr;
        g_packedBytes -= (dc->pages[victim].compSize + 31) & ~31u;
    }
}

/* Streams one page into VRAM: each LZ4 block is decoded into a 32 KB RAM buffer and sent to
   VRAM with the store queues (pvr_txr_load). The SQ API saves/restores the TA destination, so
   this is safe in the middle of a display list. */
static bool uploadPage(DcRenderer* dc, uint32_t pageId, pvr_ptr_t dst) {
    UtdcTexPage* pg = &dc->pages[pageId];
    DcPageSlot* slot = &dc->slots[pageId];
    uint8_t* payload = slot->packed;
    bool fromCache = payload != nullptr;
    if (!payload) {
        /* One sector-aligned read for the whole payload lets the CD driver DMA it in a single command */
        payload = (uint8_t*) memalign(32, (pg->compSize + 2047) & ~2047u);
        if (!payload) {
            /* low on RAM: make room by dropping cached packed pages, then retry once */
            packedCacheTrim(dc, PACKED_CACHE_BUDGET);
            payload = (uint8_t*) memalign(32, (pg->compSize + 2047) & ~2047u);
        }
        if (!payload) { logError("DC: no RAM to read page %u (%u bytes)\n", pageId, pg->compSize); return false; }
        if (fs_seek(dc->texFile, pg->offset, SEEK_SET) < 0 || fs_read(dc->texFile, payload, pg->compSize) != (ssize_t) pg->compSize) {
            logError("DC: read error on page %u\n", pageId);
            free(payload);
            return false;
        }
    }
    uint32_t outTotal = UTDC_TEX_PAGE_BYTES(pg->width, pg->height, pg->format);
    uint32_t outPos = 0, inPos = 0;
    bool ok = true;
    while (inPos < pg->compSize && outPos < outTotal) {
        UtdcTexBlock bh;
        if (pg->compSize - inPos < sizeof bh) { logError("DC: truncated page %u\n", pageId); ok = false; break; }
        memcpy(&bh, payload + inPos, sizeof bh);
        inPos += sizeof bh;
        if (bh.compSize > pg->compSize - inPos || bh.rawSize > UTDC_TEX_BLOCK_RAW || (bh.rawSize & 31)) { logError("DC: bad block header in page %u (%u/%u)\n", pageId, bh.compSize, bh.rawSize); ok = false; break; }
        int n = lz4DecodeBlock(payload + inPos, (int) bh.compSize, dc->dict, UTDC_TEX_BLOCK_RAW);
        inPos += bh.compSize;
        if (n != (int) bh.rawSize || outPos + (uint32_t) n > outTotal) { logError("DC: bad LZ4 block in page %u\n", pageId); ok = false; break; }
        pvr_txr_load(dc->dict, (uint8_t*) dst + outPos, (size_t) n);
        outPos += (uint32_t) n;
    }
    if (ok && outPos != outTotal) { logError("DC: page %u decoded %u of %u bytes\n", pageId, outPos, outTotal); ok = false; }
    slot->packedUse = ++g_packedClock;
    if (!fromCache) {
        uint32_t keep = (pg->compSize + 31) & ~31u;
        if (ok && keep <= PACKED_CACHE_BUDGET / 4) {
            packedCacheTrim(dc, keep);
            uint8_t* small = (uint8_t*) realloc(payload, keep); /* drop the sector padding */
            slot->packed = small ? small : payload;
            g_packedBytes += keep;
        } else {
            free(payload);
        }
    }
    return ok;
}

static bool ensurePage(DcRenderer* dc, uint32_t pageId) {
    if (pageId >= dc->texHdr.pageCount) return false;
    DcPageSlot* s = &dc->slots[pageId];
    s->lastUsed = dc->frame;
    if (s->vram) return true;
    if (s->retryFrame > dc->frame) return false;
    UtdcTexPage* pg = &dc->pages[pageId];
    uint32_t bytes = UTDC_TEX_PAGE_BYTES(pg->width, pg->height, pg->format);
    poolInit();
    pvr_ptr_t mem = g_poolBase ? poolAllocEvicting(dc, bytes, (int32_t) pageId) : nullptr;
    if (!mem) {
        uint32_t inUse = 0, inUseBytes = 0;
        for (uint32_t i = 0; i < dc->texHdr.pageCount; i++) if (dc->slots[i].vram && dc->slots[i].lastUsed == dc->frame) { inUse++; inUseBytes += dc->slots[i].bytes; }
        s->retryFrame = dc->frame + 1;
        logWarn("DC: out of VRAM for page %u (%u bytes): %u pages (%u KB) in use this frame, %u KB free in the pool\n", pageId, bytes, inUse, inUseBytes / 1024, (unsigned) (g_poolFree / 1024));
        return false;
    }
    uint64_t t0 = timer_us_gettime64();
    bool ok = uploadPage(dc, pageId, mem);
    uint32_t us = (uint32_t) (timer_us_gettime64() - t0);
    dc->stats.loadUsTotal += us;
    if (us > dc->stats.loadUsMax) dc->stats.loadUsMax = us;
#ifdef DC_TEXTURE_LOGS
    logInfo("DC: page %u (%ux%u, %u KB packed) loaded in %u us\n", pageId, pg->width, pg->height, pg->compSize / 1024, us);
#endif
    if (!ok) {
        logError("DC: failed to load texture page %u\n", pageId);
        poolFreePtr(mem);
        s->retryFrame = dc->frame + 30;
        return false;
    }
    s->vram = mem;
    s->bytes = bytes;
    dc->stats.pagesLoaded++;
    dc->stats.pageLoadsFrame++;
    dc->stats.pageLoadsTotal++;
    return true;
}

/* ===================================================================== */
/* Polygon headers and vertex output                                      */
/* ===================================================================== */

static int gmFactorToPvr(int32_t f, bool isDst) {
    switch (f) {
        case bm_zero: return PVR_BLEND_ZERO;
        case bm_one: return PVR_BLEND_ONE;
        case bm_src_alpha: return PVR_BLEND_SRCALPHA;
        case bm_inv_src_alpha: return PVR_BLEND_INVSRCALPHA;
        case bm_dest_alpha: return PVR_BLEND_DESTALPHA;
        case bm_inv_dest_alpha: return PVR_BLEND_INVDESTALPHA;
        /* On the PVR the "DESTCOLOR" encodings select the *other* operand's colour:
           as a source factor it means dest colour, as a dest factor it means source colour. */
        case bm_dest_color: return isDst ? PVR_BLEND_ONE : PVR_BLEND_DESTCOLOR;
        case bm_inv_dest_color: return isDst ? PVR_BLEND_ZERO : PVR_BLEND_INVDESTCOLOR;
        case bm_src_color: return isDst ? PVR_BLEND_DESTCOLOR : PVR_BLEND_ONE;
        case bm_inv_src_color: return isDst ? PVR_BLEND_INVDESTCOLOR : PVR_BLEND_ONE;
        case bm_src_alpha_sat: return PVR_BLEND_SRCALPHA;
        default: return isDst ? PVR_BLEND_INVSRCALPHA : PVR_BLEND_SRCALPHA;
    }
}

static void setState(DcRenderer* dc, int32_t pageId) {
    int src, dst;
    if (dc->blendEnabled) {
        src = gmFactorToPvr(dc->blendSrc, false);
        dst = gmFactorToPvr(dc->blendDst, true);
    } else {
        src = PVR_BLEND_ONE; dst = PVR_BLEND_ZERO;
    }
    bool filter = dc->base.texFilter;
    if (pageId == dc->curPage && src == dc->curSrc && dst == dc->curDst && filter == dc->curFilter) return;
    dc->curPage = pageId; dc->curSrc = src; dc->curDst = dst; dc->curFilter = filter;

    pvr_poly_cxt_t cxt;
    uint8_t recKind = 1;
    if (pageId >= (int32_t) dc->texHdr.pageCount) {
        DcSnapshot* sn = &g_snaps[pageId - (int32_t) dc->texHdr.pageCount];
        if (g_rollTex && sn->vram == g_rollTex) recKind = 2;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, (sn->rtt ? PVR_TXRFMT_RGB565 : PVR_TXRFMT_ARGB1555) | PVR_TXRFMT_NONTWIDDLED, sn->texW, sn->texH, sn->vram,
                         filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    } else if (pageId >= 0) {
        UtdcTexPage* pg = &dc->pages[pageId];
        int base = pg->format & ~UTDC_TEXFMT_VQ;
        int fmt = base == UTDC_TEXFMT_ARGB4444 ? PVR_TXRFMT_ARGB4444 : (base == UTDC_TEXFMT_RGB565 ? PVR_TXRFMT_RGB565 : PVR_TXRFMT_ARGB1555);
        if (pg->format & UTDC_TEXFMT_VQ) fmt |= PVR_TXRFMT_VQ_ENABLE;   /* codebook + twiddled indices */
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, fmt | PVR_TXRFMT_TWIDDLED, pg->width, pg->height, dc->slots[pageId].vram,
                         filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV; /* never wrap to the opposite page edge */
    } else {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_TWIDDLED, 8, 8, dc->whiteTex, PVR_FILTER_NONE);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    }
    cxt.gen.alpha = PVR_ALPHA_ENABLE;
    cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
    cxt.blend.src = src;
    cxt.blend.dst = dst;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_hdr_t hdr;
    pvr_poly_compile(&hdr, &cxt);
    recPrim(&hdr, recKind);
}

static inline uint32_t gmColorToArgb(uint32_t bgr, float alpha) {
    int a = (int)(alpha * 255.0f + 0.5f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return ((uint32_t) a << 24) | ((uint32_t) BGR_R(bgr) << 16) | ((uint32_t) BGR_G(bgr) << 8) | (uint32_t) BGR_B(bgr);
}

static inline void toScreen(DcRenderer* dc, float wx, float wy, float* sx, float* sy) {
    if (dc->viewRotated) {
        float dx = wx - dc->viewCX, dy = wy - dc->viewCY;
        wx = dc->viewCX + dx * dc->viewAngleCos - dy * dc->viewAngleSin;
        wy = dc->viewCY + dx * dc->viewAngleSin + dy * dc->viewAngleCos;
    }
    *sx = wx * dc->scaleX + dc->offX;
    *sy = wy * dc->scaleY + dc->offY;
}

/* Emulated window position (window_set_position): the whole picture is shifted on screen,
   like the PC window moving on the desktop (Omega Flowey intro, genocide ending). */
static float g_winDX, g_winDY;

void DcRenderer_setWindowOffset(MAYBE_UNUSED Renderer* renderer, float dx, float dy) { g_winDX = dx; g_winDY = dy; }

static inline void emitVertex(float x, float y, float u, float v, uint32_t argb, bool last) {
    pvr_vertex_t vert;
    vert.flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    /* Snap to whole screen pixels: with nearest filtering and the 2x scale this matches the PC
       (which rasterises at 1x) and keeps texel lookups away from item edges, so tiles drawn at
       sub-pixel scroll positions don't show seams between them. */
    vert.x = floorf(x + g_winDX + 0.5f); vert.y = floorf(y + g_winDY + 0.5f); vert.z = 1.0f;
    vert.u = u; vert.v = v;
    vert.argb = argb;
    vert.oargb = 0;
    recPrim(&vert, 0);
}

/* Emits a quad given 4 world-space corners in order TL, TR, BR, BL */
static void emitQuad(DcRenderer* dc, int32_t pageId, const float* wx, const float* wy, const float* u, const float* v, const uint32_t* argb) {
    if (!dc->inScene || dc->drawingToSurface || dc->stats.polysFrame >= DC_MAX_POLYS) return;
    float sx[4], sy[4];
    for (int i = 0; i < 4; i++) toScreen(dc, wx[i], wy[i], &sx[i], &sy[i]);
    float minX = fminf(fminf(sx[0], sx[1]), fminf(sx[2], sx[3]));
    float maxX = fmaxf(fmaxf(sx[0], sx[1]), fmaxf(sx[2], sx[3]));
    float minY = fminf(fminf(sy[0], sy[1]), fminf(sy[2], sy[3]));
    float maxY = fmaxf(fmaxf(sy[0], sy[1]), fmaxf(sy[2], sy[3]));
    if (maxX < 0.0f || minX > SCREEN_W || maxY < 0.0f || minY > SCREEN_H) return;
    if ((argb[0] | argb[1] | argb[2] | argb[3]) >> 24 == 0 && dc->curDst != PVR_BLEND_ZERO) return; /* fully transparent */
    setState(dc, pageId);
    /* strip order: TL, TR, BL, BR */
    emitVertex(sx[0], sy[0], u[0], v[0], argb[0], false);
    emitVertex(sx[1], sy[1], u[1], v[1], argb[1], false);
    emitVertex(sx[3], sy[3], u[3], v[3], argb[3], false);
    emitVertex(sx[2], sy[2], u[2], v[2], argb[2], true);
    dc->stats.polysFrame++;
}

static void emitTriangle(DcRenderer* dc, int32_t pageId, const float* wx, const float* wy, const float* u, const float* v, const uint32_t* argb) {
    if (!dc->inScene || dc->drawingToSurface || dc->stats.polysFrame >= DC_MAX_POLYS) return;
    float sx[3], sy[3];
    for (int i = 0; i < 3; i++) toScreen(dc, wx[i], wy[i], &sx[i], &sy[i]);
    setState(dc, pageId);
    emitVertex(sx[0], sy[0], u[0], v[0], argb[0], false);
    emitVertex(sx[1], sy[1], u[1], v[1], argb[1], false);
    emitVertex(sx[2], sy[2], u[2], v[2], argb[2], true);
    dc->stats.polysFrame++;
}

/* Resolves the Dreamcast page and UVs for a rectangle given in GameMaker
 * texture-page pixel space relative to the TPAG's source rect. */
typedef struct { int32_t page; float u0, v0, u1, v1; } DcUV;

/* Texture working-set profiler (BS_TEX_PROFILE=N: every N frames log how much of each page the
   frame actually used). Used to size page/packing decisions for heavy scenes. */
static int g_texProfEvery = -1;
static uint32_t* g_itemStamp;   /* frame an item was last counted */
static uint32_t* g_pageArea;    /* texels of distinct items used this frame */
static uint32_t* g_pageStamp;
static void texProfNote(DcRenderer* dc, uint32_t tpag, const UtdcTexItem* it) {
    if (!g_itemStamp) {
        g_itemStamp = (uint32_t*) safeCalloc(dc->texHdr.itemCount, sizeof(uint32_t));
        g_pageArea = (uint32_t*) safeCalloc(dc->texHdr.pageCount, sizeof(uint32_t));
        g_pageStamp = (uint32_t*) safeCalloc(dc->texHdr.pageCount, sizeof(uint32_t));
    }
    if (g_itemStamp[tpag] == dc->frame) return;
    g_itemStamp[tpag] = dc->frame;
    if (g_pageStamp[it->page] != dc->frame) { g_pageStamp[it->page] = dc->frame; g_pageArea[it->page] = 0; }
    g_pageArea[it->page] += (uint32_t) (it->w + 2) * (uint32_t) (it->h + 2);
}
static void texProfReport(DcRenderer* dc) {
    if (!g_texProfEvery || !g_pageStamp || dc->frame % (uint32_t) g_texProfEvery) return;
    uint32_t pages = 0, pageTexels = 0, usedTexels = 0, vramBytes = 0;
    char buf[480]; int len = 0;
    for (uint32_t i = 0; i < dc->texHdr.pageCount; i++) {
        if (g_pageStamp[i] != dc->frame) continue;
        uint32_t pt = (uint32_t) dc->pages[i].width * dc->pages[i].height;
        pages++; pageTexels += pt; usedTexels += g_pageArea[i];
        vramBytes += UTDC_TEX_PAGE_BYTES(dc->pages[i].width, dc->pages[i].height, dc->pages[i].format);
        if (len < 400) len += snprintf(buf + len, sizeof buf - (size_t) len, " %u:%u%%", (unsigned) i, (unsigned) (g_pageArea[i] * 100u / pt));
    }
    logInfo("TEXPROF f%u pages %u (%u KB VRAM) used %u Ktexels (%u%%):%s\n", (unsigned) dc->frame, pages, vramBytes / 1024, usedTexels / 1024,
            pageTexels ? (unsigned) (usedTexels * 100u / pageTexels) : 0u, buf);
}

static bool resolveUV(DcRenderer* dc, int32_t tpagIndex, float subX, float subY, float subW, float subH, DcUV* out) {
    DataWin* dw = dc->base.dataWin;
    if (tpagIndex >= (int32_t) dc->origTpagCount && (uint32_t) tpagIndex < dw->tpag.count) {
        int32_t k = tpagIndex - (int32_t) dc->origTpagCount;
        if (k >= (int32_t) arrlen(g_tpagSnap) || g_tpagSnap[k] < 0) return false;
        DcSnapshot* sn = &g_snaps[g_tpagSnap[k]];
        if (sn->pending) return false;   /* rendered after this frame */
        float iw = 1.0f / (float) sn->texW, ih = 1.0f / (float) sn->texH, is = 1.0f / sn->scale;
        out->page = (int32_t) dc->texHdr.pageCount + g_tpagSnap[k];
        out->u0 = (sn->offU + subX * is) * iw; out->v0 = (sn->offV + subY * is) * ih;
        out->u1 = (sn->offU + (subX + subW) * is) * iw; out->v1 = (sn->offV + (subY + subH) * is) * ih;
        return true;
    }
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dc->texHdr.itemCount || (uint32_t) tpagIndex >= dw->tpag.count) return false;
    if (dc->nullFrame) return false;
    UtdcTexItem* it = &dc->items[tpagIndex];
    if (it->page == UTDC_TEX_NO_PAGE) return false;
    if (g_texProfEvery) texProfNote(dc, (uint32_t) tpagIndex, it);
    if (!ensurePage(dc, it->page)) return false;
    TexturePageItem* tp = &dw->tpag.items[tpagIndex];
    float kx = tp->sourceWidth ? (float) it->w / (float) tp->sourceWidth : 1.0f;
    float ky = tp->sourceHeight ? (float) it->h / (float) tp->sourceHeight : 1.0f;
    UtdcTexPage* pg = &dc->pages[it->page];
    float iw = 1.0f / (float) pg->width, ih = 1.0f / (float) pg->height;
    out->page = it->page;
    out->u0 = ((float) it->x + subX * kx) * iw;
    out->v0 = ((float) it->y + subY * ky) * ih;
    out->u1 = ((float) it->x + (subX + subW) * kx) * iw;
    out->v1 = ((float) it->y + (subY + subH) * ky) * ih;
    return true;
}

/* ===================================================================== */
/* Vtable: frame / view                                                   */
/* ===================================================================== */

static void dcInit(Renderer* renderer, DataWin* dataWin) {
    DcRenderer* dc = (DcRenderer*) renderer;
    renderer->dataWin = dataWin;
    dc->origSpriteCount = dataWin->sprt.count;
    renderer->drawColor = 0xFFFFFF;
    renderer->drawAlpha = 1.0f;
    renderer->drawFont = -1;
    renderer->drawHalign = 0;
    renderer->drawValign = 0;
    renderer->circlePrecision = 24;
    renderer->texFilter = false;
    dc->blendEnabled = true;
    dc->blendSrc = bm_src_alpha;
    dc->blendDst = bm_inv_src_alpha;
    dc->curPage = -2;
    dc->currentTarget = APPLICATION_SURFACE_ID;
    for (int i = 0; i < MATRICES_MAX; i++) Matrix4f_identity(&renderer->gmlMatrices[i]);
    renderer->blendFactors.src = bm_src_alpha;
    renderer->blendFactors.dst = bm_inv_src_alpha;
    renderer->blendFactors.srcAlpha = bm_src_alpha;
    renderer->blendFactors.dstAlpha = bm_inv_src_alpha;
}

static void dcDestroy(Renderer* renderer) {
    DcRenderer* dc = (DcRenderer*) renderer;
    if (g_poolBase) { pvr_mem_free(g_poolBase); g_poolBase = nullptr; arrfree(g_pool); }
    if (dc->texFile != FILEHND_INVALID) fs_close(dc->texFile);
    free(dc->pages); free(dc->items); free(dc->slots); free(dc->dict);
    free(dc->surfaceW); free(dc->surfaceH); free(dc->surfaceAlive);
    free(dc);
}

static void setPortTransform(DcRenderer* dc, float viewX, float viewY, float viewW, float viewH, float portX, float portY, float portW, float portH) {
    float sx = viewW != 0.0f ? portW / viewW : 1.0f;
    float sy = viewH != 0.0f ? portH / viewH : 1.0f;
    dc->scaleX = sx * dc->winScaleX;
    dc->scaleY = sy * dc->winScaleY;
    dc->offX = (portX - viewX * sx) * dc->winScaleX;
    dc->offY = (portY - viewY * sy) * dc->winScaleY;
}

static void dcBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    DcRenderer* dc = (DcRenderer*) renderer;
    (void) windowW; (void) windowH;
    dc->gameW = gameW > 0 ? gameW : 640;
    dc->gameH = gameH > 0 ? gameH : 480;
    dc->winScaleX = SCREEN_W / (float) dc->gameW;
    dc->winScaleY = SCREEN_H / (float) dc->gameH;
    dc->viewRotated = false;
    setPortTransform(dc, 0, 0, (float) dc->gameW, (float) dc->gameH, 0, 0, (float) dc->gameW, (float) dc->gameH);
}

static void dcEndFrameInit(MAYBE_UNUSED Renderer* renderer) {}
static void dcEndFrameEnd(MAYBE_UNUSED Renderer* renderer) {}

static void dcBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    DcRenderer* dc = (DcRenderer*) renderer;
    setPortTransform(dc, (float) viewX, (float) viewY, (float) viewW, (float) viewH, (float) portX, (float) portY, (float) portW, (float) portH);
    dc->viewRotated = viewAngle != 0.0f;
    if (dc->viewRotated) {
        float a = viewAngle * ((float) M_PI / 180.0f);
        dc->viewAngleCos = cosf(a); dc->viewAngleSin = sinf(a);
        dc->viewCX = (float) viewX + (float) viewW * 0.5f;
        dc->viewCY = (float) viewY + (float) viewH * 0.5f;
    }
}

static void dcEndView(MAYBE_UNUSED Renderer* renderer) {}

static void dcApplyProjection(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED const Matrix4f* viewMatrix, MAYBE_UNUSED const Matrix4f* projectionMatrix) {}

static void dcBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, MAYBE_UNUSED int32_t targetSurfaceId) {
    DcRenderer* dc = (DcRenderer*) renderer;
    dc->viewRotated = false;
    setPortTransform(dc, 0, 0, (float) guiW, (float) guiH, (float) portX, (float) portY, (float) portW, (float) portH);
}

static void dcSetGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portW, int32_t portH, MAYBE_UNUSED bool renderingToUserSurface) {
    DcRenderer* dc = (DcRenderer*) renderer;
    dc->viewRotated = false;
    setPortTransform(dc, 0, 0, (float) guiW, (float) guiH, 0, 0, (float) portW, (float) portH);
}

static void dcEndGUI(MAYBE_UNUSED Renderer* renderer) {}

/* ===================================================================== */
/* Sprites                                                                */
/* ===================================================================== */

static void dcDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    DcRenderer* dc = (DcRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    TexturePageItem* tp = &dw->tpag.items[tpagIndex];
    DcUV uv;
    if (!resolveUV(dc, tpagIndex, 0, 0, tp->sourceWidth, tp->sourceHeight, &uv)) return;

    float lx0 = (float) tp->targetX - originX, ly0 = (float) tp->targetY - originY;
    float lx1 = lx0 + (float) tp->targetWidth, ly1 = ly0 + (float) tp->targetHeight;
    float wx[4], wy[4];
    if (angleDeg == 0.0f) {
        wx[0] = x + lx0 * xscale; wy[0] = y + ly0 * yscale;
        wx[1] = x + lx1 * xscale; wy[1] = wy[0];
        wx[2] = wx[1];            wy[2] = y + ly1 * yscale;
        wx[3] = wx[0];            wy[3] = wy[2];
    } else {
        float a = -angleDeg * ((float) M_PI / 180.0f);
        float c = cosf(a), s = sinf(a);
        float lx[4] = { lx0 * xscale, lx1 * xscale, lx1 * xscale, lx0 * xscale };
        float ly[4] = { ly0 * yscale, ly0 * yscale, ly1 * yscale, ly1 * yscale };
        for (int i = 0; i < 4; i++) { wx[i] = x + lx[i] * c - ly[i] * s; wy[i] = y + lx[i] * s + ly[i] * c; }
    }
    float u[4] = { uv.u0, uv.u1, uv.u1, uv.u0 };
    float v[4] = { uv.v0, uv.v0, uv.v1, uv.v1 };
    uint32_t c = gmColorToArgb(color, alpha);
    uint32_t cs[4] = { c, c, c, c };
    emitQuad(dc, uv.page, wx, wy, u, v, cs);
}

static void dcDrawSpritePartColor(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha) {
    DcRenderer* dc = (DcRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    DcUV uv;
    if (!resolveUV(dc, tpagIndex, srcOffX, srcOffY, srcW, srcH, &uv)) return;
    float wx[4] = { x, x + srcW * xscale, x + srcW * xscale, x };
    float wy[4] = { y, y, y + srcH * yscale, y + srcH * yscale };
    if (angleDeg != 0.0f) {
        float a = -angleDeg * ((float) M_PI / 180.0f);
        float c = cosf(a), s = sinf(a);
        for (int i = 0; i < 4; i++) {
            float dx = wx[i] - pivotX, dy = wy[i] - pivotY;
            wx[i] = c * dx - s * dy + pivotX;
            wy[i] = s * dx + c * dy + pivotY;
        }
    }
    float u[4] = { uv.u0, uv.u1, uv.u1, uv.u0 };
    float v[4] = { uv.v0, uv.v0, uv.v1, uv.v1 };
    uint32_t cs[4] = { gmColorToArgb(color1, alpha), gmColorToArgb(color2, alpha), gmColorToArgb(color3, alpha), gmColorToArgb(color4, alpha) };
    emitQuad(dc, uv.page, wx, wy, u, v, cs);
}

static void dcDrawSpritePart(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    dcDrawSpritePartColor(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color, color, color, color, alpha);
}

static void dcDrawSpritePos(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
    DcRenderer* dc = (DcRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    TexturePageItem* tp = &dw->tpag.items[tpagIndex];
    DcUV uv;
    if (!resolveUV(dc, tpagIndex, 0, 0, tp->sourceWidth, tp->sourceHeight, &uv)) return;
    float wx[4] = { x1, x2, x3, x4 }, wy[4] = { y1, y2, y3, y4 };
    float u[4] = { uv.u0, uv.u1, uv.u1, uv.u0 };
    float v[4] = { uv.v0, uv.v0, uv.v1, uv.v1 };
    uint32_t c = gmColorToArgb(0xFFFFFF, alpha);
    uint32_t cs[4] = { c, c, c, c };
    emitQuad(dc, uv.page, wx, wy, u, v, cs);
}

static void dcDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
    DcRenderer* dc = (DcRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    TexturePageItem* tp = &dw->tpag.items[tpagIndex];
    DcUV uv;
    if (!resolveUV(dc, tpagIndex, 0, 0, tp->sourceWidth, tp->sourceHeight, &uv)) return;
    float ax = fabsf(xscale), ay = fabsf(yscale);
    float tileW = (float) tp->boundingWidth * ax, tileH = (float) tp->boundingHeight * ay;
    if (tileW <= 0 || tileH <= 0) return;
    float startX, endX, startY, endY;
    if (tileX) { startX = fmodf(x - originX * ax, tileW); if (startX > 0) startX -= tileW; endX = roomW; }
    else { startX = x - originX * ax; endX = startX + tileW; }
    if (tileY) { startY = fmodf(y - originY * ay, tileH); if (startY > 0) startY -= tileH; endY = roomH; }
    else { startY = y - originY * ay; endY = startY + tileH; }
    float lx0 = (float) tp->targetX - originX, ly0 = (float) tp->targetY - originY;
    float lx1 = lx0 + tp->targetWidth, ly1 = ly0 + tp->targetHeight;
    float sx0 = xscale * lx0, sy0 = yscale * ly0, sx1 = xscale * lx1, sy1 = yscale * ly1;
    float u[4] = { uv.u0, uv.u1, uv.u1, uv.u0 };
    float v[4] = { uv.v0, uv.v0, uv.v1, uv.v1 };
    uint32_t c = gmColorToArgb(color, alpha);
    uint32_t cs[4] = { c, c, c, c };
    int guard = 0;
    for (float dy = startY; endY > dy && guard < 4096; dy += tileH) {
        float cy = dy + originY * ay;
        for (float dx = startX; endX > dx && guard < 4096; dx += tileW, guard++) {
            float cx = dx + originX * ax;
            float wx[4] = { cx + sx0, cx + sx1, cx + sx1, cx + sx0 };
            float wy[4] = { cy + sy0, cy + sy0, cy + sy1, cy + sy1 };
            emitQuad(dc, uv.page, wx, wy, u, v, cs);
        }
    }
}

/* ===================================================================== */
/* Shapes                                                                 */
/* ===================================================================== */

static void solidQuad(DcRenderer* dc, const float* wx, const float* wy, const uint32_t* argb) {
    float u[4] = { 0.5f, 0.5f, 0.5f, 0.5f }, v[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
    emitQuad(dc, -1, wx, wy, u, v, argb);
}

static void lineQuad(DcRenderer* dc, float x1, float y1, float x2, float y2, float width, uint32_t c1, uint32_t c2) {
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) { dx = 1; dy = 0; len = 1; }
    float w = width < 1.0f ? 1.0f : width;
    float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
    float wx[4] = { x1 + nx, x2 + nx, x2 - nx, x1 - nx };
    float wy[4] = { y1 + ny, y2 + ny, y2 - ny, y1 - ny };
    uint32_t cs[4] = { c1, c2, c2, c1 };
    solidQuad(dc, wx, wy, cs);
}

static void dcDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha, bool outline) {
    DcRenderer* dc = (DcRenderer*) renderer;
    if (x1 > x2) { float t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { float t = y1; y1 = y2; y2 = t; }
    uint32_t c1 = gmColorToArgb(color1, alpha), c2 = gmColorToArgb(color2, alpha), c3 = gmColorToArgb(color3, alpha), c4 = gmColorToArgb(color4, alpha);
    if (outline) {
        /* GameMaker outlines are 1 pixel wide and inclusive of both corners */
        float wxT[4] = { x1, x2 + 1, x2 + 1, x1 }, wyT[4] = { y1, y1, y1 + 1, y1 + 1 };
        uint32_t cT[4] = { c1, c2, c2, c1 };
        solidQuad(dc, wxT, wyT, cT);
        float wxB[4] = { x1, x2 + 1, x2 + 1, x1 }, wyB[4] = { y2, y2, y2 + 1, y2 + 1 };
        uint32_t cB[4] = { c4, c3, c3, c4 };
        solidQuad(dc, wxB, wyB, cB);
        float wxL[4] = { x1, x1 + 1, x1 + 1, x1 }, wyL[4] = { y1 + 1, y1 + 1, y2, y2 };
        uint32_t cL[4] = { c1, c1, c4, c4 };
        solidQuad(dc, wxL, wyL, cL);
        float wxR[4] = { x2, x2 + 1, x2 + 1, x2 }, wyR[4] = { y1 + 1, y1 + 1, y2, y2 };
        uint32_t cR[4] = { c2, c2, c3, c3 };
        solidQuad(dc, wxR, wyR, cR);
        return;
    }
    float wx[4] = { x1, x2 + 1, x2 + 1, x1 }, wy[4] = { y1, y1, y2 + 1, y2 + 1 };
    uint32_t cs[4] = { c1, c2, c3, c4 };
    solidQuad(dc, wx, wy, cs);
}

static void dcDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    dcDrawRectangleColor(renderer, x1, y1, x2, y2, color, color, color, color, alpha, outline);
}

static void dcDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    lineQuad((DcRenderer*) renderer, x1, y1, x2, y2, width, gmColorToArgb(color1, alpha), gmColorToArgb(color2, alpha));
}

static void dcDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    dcDrawLineColor(renderer, x1, y1, x2, y2, width, color, color, alpha);
}

static void dcDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha, bool outline) {
    DcRenderer* dc = (DcRenderer*) renderer;
    uint32_t c1 = gmColorToArgb(color1, alpha), c2 = gmColorToArgb(color2, alpha), c3 = gmColorToArgb(color3, alpha);
    if (outline) {
        lineQuad(dc, x1, y1, x2, y2, 1, c1, c2);
        lineQuad(dc, x2, y2, x3, y3, 1, c2, c3);
        lineQuad(dc, x3, y3, x1, y1, 1, c3, c1);
        return;
    }
    float wx[3] = { x1, x2, x3 }, wy[3] = { y1, y2, y3 };
    float u[3] = { 0.5f, 0.5f, 0.5f }, v[3] = { 0.5f, 0.5f, 0.5f };
    uint32_t cs[3] = { c1, c2, c3 };
    emitTriangle(dc, -1, wx, wy, u, v, cs);
}

/* ===================================================================== */
/* Primitives (draw_primitive_begin / draw_vertex)                        */
/* ===================================================================== */

static void dcPrimitiveBegin(Renderer* renderer, int32_t primitiveType) {
    DcRenderer* dc = (DcRenderer*) renderer;
    dc->primType = primitiveType; dc->primTexture = -1; dc->primCount = 0;
}

static void dcPrimitiveBeginTexture(Renderer* renderer, int32_t primitiveType, int32_t texture) {
    DcRenderer* dc = (DcRenderer*) renderer;
    dc->primType = primitiveType; dc->primTexture = texture; dc->primCount = 0;
}

static void dcDrawVertex(Renderer* renderer, float x, float y, MAYBE_UNUSED float z, uint32_t color, float alpha, float u, float v) {
    DcRenderer* dc = (DcRenderer*) renderer;
    if (dc->primCount >= MAX_PRIM_VERTS) return;
    int i = dc->primCount++;
    dc->primX[i] = x; dc->primY[i] = y; dc->primU[i] = u; dc->primV[i] = v;
    dc->primC[i] = gmColorToArgb(color, alpha);
}

static void primTri(DcRenderer* dc, int32_t page, DcUV* uv, int a, int b, int c) {
    float wx[3] = { dc->primX[a], dc->primX[b], dc->primX[c] };
    float wy[3] = { dc->primY[a], dc->primY[b], dc->primY[c] };
    float u[3], v[3];
    int idx[3] = { a, b, c };
    for (int k = 0; k < 3; k++) {
        if (uv) { u[k] = uv->u0 + (uv->u1 - uv->u0) * dc->primU[idx[k]]; v[k] = uv->v0 + (uv->v1 - uv->v0) * dc->primV[idx[k]]; }
        else { u[k] = 0.5f; v[k] = 0.5f; }
    }
    uint32_t cs[3] = { dc->primC[a], dc->primC[b], dc->primC[c] };
    emitTriangle(dc, page, wx, wy, u, v, cs);
}

static void dcPrimitiveEnd(Renderer* renderer) {
    DcRenderer* dc = (DcRenderer*) renderer;
    int n = dc->primCount;
    int32_t page = -1;
    DcUV uv, *uvp = nullptr;
    if (dc->primTexture >= 0) {
        /* texture handles are TPAG indices (see dcSpriteGetTexture) */
        TexturePageItem* tp = (uint32_t) dc->primTexture < renderer->dataWin->tpag.count ? &renderer->dataWin->tpag.items[dc->primTexture] : nullptr;
        if (tp && resolveUV(dc, dc->primTexture, 0, 0, tp->sourceWidth, tp->sourceHeight, &uv)) { page = uv.page; uvp = &uv; }
    }
    switch (dc->primType) {
        case PRIMITIVE_TRIANGLES:
            for (int i = 0; i + 2 < n; i += 3) primTri(dc, page, uvp, i, i + 1, i + 2);
            break;
        case PRIMITIVE_TRIANGLE_STRIP:
            for (int i = 0; i + 2 < n; i++) primTri(dc, page, uvp, i, i + 1, i + 2);
            break;
        case PRIMITIVE_TRIANGLE_FAN:
            for (int i = 1; i + 1 < n; i++) primTri(dc, page, uvp, 0, i, i + 1);
            break;
        case PRIMITIVE_LINES:
            for (int i = 0; i + 1 < n; i += 2) lineQuad(dc, dc->primX[i], dc->primY[i], dc->primX[i + 1], dc->primY[i + 1], 1, dc->primC[i], dc->primC[i + 1]);
            break;
        case PRIMITIVE_LINE_STRIP:
            for (int i = 0; i + 1 < n; i++) lineQuad(dc, dc->primX[i], dc->primY[i], dc->primX[i + 1], dc->primY[i + 1], 1, dc->primC[i], dc->primC[i + 1]);
            break;
        case PRIMITIVE_POINTS:
            for (int i = 0; i < n; i++) {
                float wx[4] = { dc->primX[i], dc->primX[i] + 1, dc->primX[i] + 1, dc->primX[i] };
                float wy[4] = { dc->primY[i], dc->primY[i], dc->primY[i] + 1, dc->primY[i] + 1 };
                uint32_t cs[4] = { dc->primC[i], dc->primC[i], dc->primC[i], dc->primC[i] };
                solidQuad(dc, wx, wy, cs);
            }
            break;
        default: break;
    }
    dc->primCount = 0;
}

static void dcDrawVertexBuffer(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED VertexBuffer* buffer, MAYBE_UNUSED int32_t primitive, MAYBE_UNUSED int32_t texture, MAYBE_UNUSED int32_t offset, MAYBE_UNUSED int32_t count) {}

/* ===================================================================== */
/* Text                                                                   */
/* ===================================================================== */

static void drawTextImpl(DcRenderer* dc, Font* font, const char* text, float x, float y, float xscale, float yscale, float angleDeg,
                         int32_t c1in, int32_t c2in, int32_t c3in, int32_t c4in, float alpha, float lineSeparation, bool gradient) {
    Renderer* renderer = &dc->base;
    DataWin* dw = renderer->dataWin;
    if (text == nullptr || font == nullptr) return;
    Sprite* spriteFont = nullptr;
    int32_t fontTpag = -1;
    if (!font->isSpriteFont) {
        fontTpag = font->tpagIndex;
        if (fontTpag < 0) return;
    } else if (font->spriteIndex >= 0 && dw->sprt.count > (uint32_t) font->spriteIndex) {
        spriteFont = &dw->sprt.sprites[font->spriteIndex];
    } else return;

    int32_t textLen = (int32_t) strlen(text);
    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));
    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float sxs = xscale * font->scaleX, sys = yscale * font->scaleY;
    float a = -angleDeg * ((float) M_PI / 180.0f);
    float ca = cosf(a), sa = sinf(a);
    bool rot = angleDeg != 0.0f;

    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;
    uint32_t solid = gmColorToArgb((uint32_t) c1in, alpha);

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;
        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float cursorX = 0;
        if (renderer->drawHalign == 1) cursorX = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) cursorX = -lineWidth;
        float gradientX = 0;

        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) { ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos); hasCh = true; }
        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            if (glyph != nullptr) {
                bool drew = false;
                if (ch == ' ') drew = true;
                else if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    DcUV uv;
                    bool ok;
                    float lx0, ly0;
                    if (spriteFont) {
                        int32_t gi = (int32_t) (glyph - font->glyphs);
                        ok = gi >= 0 && gi < (int32_t) spriteFont->textureCount;
                        int32_t ti = ok ? spriteFont->tpagIndices[gi] : -1;
                        TexturePageItem* gt = ok && ti >= 0 ? &dw->tpag.items[ti] : nullptr;
                        ok = gt && resolveUV(dc, ti, 0, 0, gt->sourceWidth, gt->sourceHeight, &uv);
                        lx0 = cursorX + (float) glyph->offset;
                        ly0 = cursorY + (gt ? (float) (int32_t) gt->targetY : 0.0f) - (float) font->spriteOriginYAdjust;
                    } else {
                        ok = resolveUV(dc, fontTpag, glyph->sourceX, glyph->sourceY, glyph->sourceWidth, glyph->sourceHeight, &uv);
                        lx0 = cursorX + glyph->offset;
                        ly0 = cursorY + DefaultFont_glyphYOffset(font, glyph->character);
                    }
                    if (ok) {
                        float lx1 = lx0 + (float) glyph->sourceWidth, ly1 = ly0 + (float) glyph->sourceHeight;
                        float lx[4] = { lx0 * sxs, lx1 * sxs, lx1 * sxs, lx0 * sxs };
                        float ly[4] = { ly0 * sys, ly0 * sys, ly1 * sys, ly1 * sys };
                        float wx[4], wy[4];
                        for (int k = 0; k < 4; k++) {
                            if (rot) { wx[k] = x + lx[k] * ca - ly[k] * sa; wy[k] = y + lx[k] * sa + ly[k] * ca; }
                            else { wx[k] = x + lx[k]; wy[k] = y + ly[k]; }
                        }
                        float u[4] = { uv.u0, uv.u1, uv.u1, uv.u0 };
                        float v[4] = { uv.v0, uv.v0, uv.v1, uv.v1 };
                        uint32_t cs[4];
                        if (gradient) {
                            float adv = (float) glyph->shift;
                            float lf = lineWidth > 0 ? gradientX / lineWidth : 0.0f;
                            float rf = lineWidth > 0 ? (gradientX + adv) / lineWidth : 1.0f;
                            cs[0] = gmColorToArgb((uint32_t) Color_lerp(c1in, c2in, lf), alpha);
                            cs[1] = gmColorToArgb((uint32_t) Color_lerp(c1in, c2in, rf), alpha);
                            cs[2] = gmColorToArgb((uint32_t) Color_lerp(c4in, c3in, rf), alpha);
                            cs[3] = gmColorToArgb((uint32_t) Color_lerp(c4in, c3in, lf), alpha);
                        } else {
                            cs[0] = cs[1] = cs[2] = cs[3] = solid;
                        }
                        emitQuad(dc, uv.page, wx, wy, u, v, cs);
                        drew = true;
                    }
                }
                cursorX += glyph->shift;
                gradientX += glyph->shift;
                if (drew && hasNext) cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
            ch = nextCh;
            hasCh = hasNext;
        }
        cursorY += lineStride;
        if (textLen > lineEnd) lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        else lineStart = lineEnd;
    }
}

static Font* currentFont(Renderer* renderer) {
    DataWin* dw = renderer->dataWin;
    int32_t fi = renderer->drawFont;
    if (dw != nullptr && fi >= 0 && dw->font.count > (uint32_t) fi) return &dw->font.fonts[fi];
    return nullptr;
}

static void dcDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, float lineSeparation) {
    drawTextImpl((DcRenderer*) renderer, currentFont(renderer), text, x, y, xscale, yscale, angleDeg,
                 (int32_t) renderer->drawColor, 0, 0, 0, renderer->drawAlpha, lineSeparation, false);
}

static void dcDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    drawTextImpl((DcRenderer*) renderer, currentFont(renderer), text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation, true);
}

static void dcDrawTextUI(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED const char* text, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale, MAYBE_UNUSED float angleDeg, MAYBE_UNUSED int32_t c1, MAYBE_UNUSED int32_t c2, MAYBE_UNUSED int32_t c3, MAYBE_UNUSED int32_t c4, MAYBE_UNUSED float alpha, MAYBE_UNUSED float lineSeparation) {
    /* The runner's built-in debug UI font is not shipped on the disc. */
}

/* ===================================================================== */
/* Misc state                                                             */
/* ===================================================================== */

static void dcFlush(MAYBE_UNUSED Renderer* renderer) {}

static void dcClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    DcRenderer* dc = (DcRenderer*) renderer;
    /* Full-screen quad with blending off, in screen space */
    bool savedBlend = dc->blendEnabled;
    dc->blendEnabled = false;
    float sx = dc->scaleX, sy = dc->scaleY, ox = dc->offX, oy = dc->offY;
    bool rot = dc->viewRotated;
    dc->scaleX = dc->scaleY = 1.0f; dc->offX = dc->offY = 0.0f; dc->viewRotated = false;
    float wx[4] = { 0, SCREEN_W, SCREEN_W, 0 }, wy[4] = { 0, 0, SCREEN_H, SCREEN_H };
    uint32_t c = gmColorToArgb(color, alpha);
    uint32_t cs[4] = { c, c, c, c };
    solidQuad(dc, wx, wy, cs);
    dc->scaleX = sx; dc->scaleY = sy; dc->offX = ox; dc->offY = oy; dc->viewRotated = rot;
    dc->blendEnabled = savedBlend;
}

static uint32_t nextPow2(uint32_t v) { uint32_t p = 8; while (p < v) p <<= 1; return p; }

/* Wraps snapshot slot snapIdx in a synthetic TPAG + sprite (w x h sprite pixels). */
static int32_t makeSnapshotSprite(DcRenderer* dc, DataWin* dw, int32_t snapIdx, int32_t w, int32_t h, int32_t xorig, int32_t yorig) {
    /* synthetic TPAG */
    uint32_t tpagIndex = 0xFFFFFFFFu;
    for (uint32_t i = dc->origTpagCount; i < dw->tpag.count; i++) {
        int32_t k = (int32_t) (i - dc->origTpagCount);
        if (k < (int32_t) arrlen(g_tpagSnap) && g_tpagSnap[k] < 0) { tpagIndex = i; break; }
    }
    if (tpagIndex == 0xFFFFFFFFu) {
        tpagIndex = dw->tpag.count++;
        dw->tpag.items = (TexturePageItem*) safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
        int32_t none = -1;
        while ((int32_t) arrlen(g_tpagSnap) <= (int32_t) (tpagIndex - dc->origTpagCount)) arrput(g_tpagSnap, none);
    }
    TexturePageItem* tp = &dw->tpag.items[tpagIndex];
    memset(tp, 0, sizeof *tp);
    tp->sourceWidth = tp->targetWidth = tp->boundingWidth = (uint16_t) w;
    tp->sourceHeight = tp->targetHeight = tp->boundingHeight = (uint16_t) h;
    tp->texturePageId = 0;
    g_tpagSnap[tpagIndex - dc->origTpagCount] = snapIdx;

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, dc->origSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    sprite->width = (uint32_t) w;
    sprite->height = (uint32_t) h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t*) safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t) tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = nullptr;
    return (int32_t) spriteIndex;
}

static int32_t allocSnapSlot(void) {
    int32_t snapIdx = -1;
    for (int32_t k = 0; k < (int32_t) arrlen(g_snaps); k++) if (!g_snaps[k].used) { snapIdx = k; break; }
    if (snapIdx < 0) { DcSnapshot z = {0}; arrput(g_snaps, z); snapIdx = (int32_t) arrlen(g_snaps) - 1; }
    DcSnapshot* sn = &g_snaps[snapIdx];
    memset(sn, 0, sizeof *sn);
    sn->used = true; sn->scale = 2.0f;
    return snapIdx;
}

/* Render-to-texture capture (framebuffer not readable): see the comment at g_fbReadable. */
static int32_t rttSnapshot(DcRenderer* dc, DataWin* dw, int32_t x, int32_t y, int32_t w, int32_t h, int32_t xorig, int32_t yorig) {
    g_lastSnapReq = dc->frame;
    int32_t fx = x + (int32_t) g_winDX, fy = y + (int32_t) g_winDY;
    int32_t snapIdx;
    if (g_rollTex && g_rollValidFrame + 1 == dc->frame) {
        /* the rolling capture holds the previous frame: point into it */
        snapIdx = allocSnapSlot();
        DcSnapshot* sn = &g_snaps[snapIdx];
        sn->rtt = true; sn->live = true; sn->ownsVram = false;
        sn->vram = g_rollTex; sn->texW = ROLL_W; sn->texH = ROLL_H;
        sn->offU = (float) fx * 0.5f; sn->offV = (float) fy * 0.5f;
    } else {
        uint32_t sw = (uint32_t) (w + 1) / 2, sh = (uint32_t) (h + 1) / 2;
        if (sw > 512) sw = 512;
        if (sh > 256) sh = 256;
        uint32_t texW = nextPow2(sw), texH = nextPow2(sh);
        uint32_t bytes = texW * texH * 2;
        pvr_ptr_t mem = nullptr;
        if (pvr_mem_available() >= bytes + 4096) mem = pvr_mem_malloc(bytes);
        if (!mem) { logWarn("DC: no VRAM for a %ux%u screen capture\n", texW, texH); return -1; }
        snapIdx = allocSnapSlot();
        DcSnapshot* sn = &g_snaps[snapIdx];
        sn->rtt = true; sn->pending = true; sn->ownsVram = true;
        sn->vram = mem; sn->texW = (uint16_t) texW; sn->texH = (uint16_t) texH;
    }
    DcSnapshot* sn = &g_snaps[snapIdx];
    sn->rectX = (int16_t) fx; sn->rectY = (int16_t) fy; sn->rectW = (uint16_t) w; sn->rectH = (uint16_t) h;
    return makeSnapshotSprite(dc, dw, snapIdx, w, h, xorig, yorig);
}

static int32_t dcCreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, MAYBE_UNUSED bool smooth, int32_t xorig, int32_t yorig) {
    DcRenderer* dc = (DcRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (surfaceID != APPLICATION_SURFACE_ID || w <= 0 || h <= 0) {
        static int warned;
        if (warned++ < 3) logInfo("DC: sprite_create_from_surface(%d, %d, %d, %d, %d) not supported\n", (int) surfaceID, (int) x, (int) y, (int) w, (int) h);
        return -1;
    }
    if (g_fbReadable == 0) return rttSnapshot(dc, dw, x, y, w, h, xorig, yorig);
    uint64_t capT0 = timer_us_gettime64();

    /* application_surface is the 640x480 window picture, i.e. the framebuffer (minus the
       emulated window shake). Clip the rect, sample every other pixel. */
    int32_t fx = x + (int32_t) g_winDX, fy = y + (int32_t) g_winDY;
    uint32_t sw = (uint32_t) (w + 1) / 2, sh = (uint32_t) (h + 1) / 2;
    if (sw > 512) sw = 512;
    if (sh > 256) sh = 256;
    uint32_t texW = nextPow2(sw), texH = nextPow2(sh);
    uint32_t bytes = texW * texH * 2;
    pvr_ptr_t mem = nullptr;
    if (pvr_mem_available() >= bytes + 4096) mem = pvr_mem_malloc(bytes);
    if (!mem) { logWarn("DC: no VRAM for a %ux%u screen snapshot\n", texW, texH); return -1; }
    uint32_t fbOff = PVR_GET(PVR_FB_ADDR) & 0x007FFFFFu;
    /* Read the framebuffer through the cache (P1 mirror of the 32-bit VRAM area) so each 32-byte
       line comes in one burst; invalidate first so no stale lines are used. */
    const uint8_t* fbBase = (const uint8_t*) (0x85000000u + fbOff);
    static uint16_t rowBuf[512] __attribute__((aligned(32)));
    uint16_t key = 0;
    bool haveKey = false;
    uint16_t firstTexel = 0; bool uniform = true; bool firstSeen = false;
    uint32_t rowBytes = (sw * 2 + 31) & ~31u;
    if (rowBytes > texW * 2) rowBytes = texW * 2;
    for (uint32_t j = 0; j < sh; j++) {
        int32_t sy = fy + (int32_t) j * 2;
        memset(rowBuf, 0, sizeof rowBuf);
        if (sy >= 0 && sy < 480) {
            const uint8_t* line = fbBase + (uint32_t) sy * 1280u;
            dcache_inval_range((uintptr_t) line & ~31u, 1280 + 32);
            const uint16_t* src = (const uint16_t*) line;
            int32_t i0 = fx < 0 ? (-fx + 1) / 2 : 0;
            for (int32_t i = i0; i < (int32_t) sw; i++) {
                int32_t sx = fx + i * 2;
                if (sx >= 640) break;
                uint16_t p565 = src[sx];
                if (!firstSeen) { firstTexel = p565; firstSeen = true; } else if (p565 != firstTexel) uniform = false;
                uint16_t c = (uint16_t) (0x8000 | ((p565 >> 1) & 0x7FE0) | (p565 & 0x1F)); /* RGB565 -> ARGB1555 */
                if (removeback) {
                    if (!haveKey) { key = c; haveKey = true; }
                    if (c == key) c = 0;
                }
                rowBuf[i] = c;
            }
        }
        uint8_t* dstRow = (uint8_t*) mem + j * texW * 2;
        if (rowBytes >= 32) pvr_txr_load(rowBuf, dstRow, rowBytes);
        else { volatile uint32_t* d = (volatile uint32_t*) dstRow; for (uint32_t i = 0; i < texW / 2; i++) d[i] = (uint32_t) rowBuf[2 * i] | ((uint32_t) rowBuf[2 * i + 1] << 16); }
    }

    {
        static uint32_t capCount;
        if ((capCount++ % 60) == 0)
            logInfo("DC: screen snapshot %dx%d at (%d,%d) took %u us%s\n", (int) w, (int) h, (int) x, (int) y, (unsigned) (timer_us_gettime64() - capT0),
                    uniform ? " (flat colour: framebuffer not readable here?)" : "");
    }
    /* A capture of a real scene is never one flat colour. When it is, the framebuffer can't be
       read back (emulators that render on the host GPU only copy it to emulated VRAM when asked
       to); report that so the caller can fall back instead of showing a black box. Real hardware
       always reads the real picture. */
    static int forceUnreadable = -1;   /* test switch: BS_SNAPSHOT_UNREADABLE=1 */
    if (forceUnreadable < 0) forceUnreadable = getenv("BS_SNAPSHOT_UNREADABLE") != nullptr;
    if ((uniform || forceUnreadable) && w >= 16 && h >= 16) { pvr_mem_free(mem); return SPRITE_SNAPSHOT_UNREADABLE; }
    /* snapshot slot */
    int32_t snapIdx = allocSnapSlot();
    DcSnapshot* sn = &g_snaps[snapIdx];
    sn->vram = mem; sn->texW = (uint16_t) texW; sn->texH = (uint16_t) texH; sn->ownsVram = true;
    return makeSnapshotSprite(dc, dw, snapIdx, w, h, xorig, yorig);
}

static void dcDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    DcRenderer* dc = (DcRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (spriteIndex < (int32_t) dc->origSpriteCount || (uint32_t) spriteIndex >= dw->sprt.count) return;
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;
    for (uint32_t i = 0; i < sprite->textureCount; i++) {
        int32_t t = sprite->tpagIndices[i];
        if (t < (int32_t) dc->origTpagCount) continue;
        int32_t k = t - (int32_t) dc->origTpagCount;
        if (k < (int32_t) arrlen(g_tpagSnap) && g_tpagSnap[k] >= 0) {
            /* the PVR may still be drawing it: free two frames later */
            g_snaps[g_tpagSnap[k]].freeAtFrame = dc->frame + 2;
            g_tpagSnap[k] = -1;
        }
    }
    free(sprite->tpagIndices);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;
}

static void freeOldSnapshots(DcRenderer* dc) {
    for (int32_t k = 0; k < (int32_t) arrlen(g_snaps); k++) {
        DcSnapshot* sn = &g_snaps[k];
        if (sn->used && sn->freeAtFrame && dc->frame >= sn->freeAtFrame) {
            if (sn->ownsVram) pvr_mem_free(sn->vram);
            sn->vram = nullptr; sn->used = false; sn->freeAtFrame = 0;
        }
    }
}

static BlendFactors dcGpuGetBlendFactors(Renderer* renderer) { return renderer->blendFactors; }

static int32_t dcGpuGetBlendMode(Renderer* renderer) {
    DcRenderer* dc = (DcRenderer*) renderer;
    if (dc->blendSrc == bm_src_alpha && dc->blendDst == bm_one) return bm_add;
    if (dc->blendSrc == bm_src_alpha && dc->blendDst == bm_inv_src_alpha) return bm_normal;
    if (dc->blendSrc == bm_zero && dc->blendDst == bm_inv_src_color) return bm_subtract;
    return bm_normal;
}

static void applyBlend(DcRenderer* dc, int32_t src, int32_t dst) {
    dc->blendSrc = src; dc->blendDst = dst;
    dc->base.blendFactors.src = src; dc->base.blendFactors.dst = dst;
    dc->base.blendFactors.srcAlpha = src; dc->base.blendFactors.dstAlpha = dst;
}

static void dcGpuSetBlendMode(Renderer* renderer, int32_t mode) {
    DcRenderer* dc = (DcRenderer*) renderer;
    switch (mode) {
        case bm_add: applyBlend(dc, bm_src_alpha, bm_one); break;
        case bm_subtract: case bm_reverse_subtract: applyBlend(dc, bm_zero, bm_inv_src_color); break;
        case bm_max: applyBlend(dc, bm_src_alpha, bm_inv_src_color); break;
        default: applyBlend(dc, bm_src_alpha, bm_inv_src_alpha); break;
    }
}

static void dcGpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor, MAYBE_UNUSED int32_t sfactor_alpha, MAYBE_UNUSED int32_t dfactor_alpha) {
    applyBlend((DcRenderer*) renderer, sfactor, dfactor);
}

static void dcGpuSetBlendEnable(Renderer* renderer, bool enable) { ((DcRenderer*) renderer)->blendEnabled = enable; }
static bool dcGpuGetBlendEnable(Renderer* renderer) { return ((DcRenderer*) renderer)->blendEnabled; }
static void dcGpuSetTexFilter(Renderer* renderer, bool enable) { renderer->texFilter = enable; }
static void dcGpuSetAlphaTestEnable(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED bool enable) {}
static bool dcGpuGetAlphaTestEnable(MAYBE_UNUSED Renderer* renderer) { return false; }
static void dcGpuSetAlphaTestRef(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED uint8_t ref) {}
static void dcGpuSetColorWriteEnable(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED bool r, MAYBE_UNUSED bool g, MAYBE_UNUSED bool b, MAYBE_UNUSED bool a) {}
static void dcGpuGetColorWriteEnable(MAYBE_UNUSED Renderer* renderer, bool* r, bool* g, bool* b, bool* a) { *r = *g = *b = *a = true; }
static void dcGpuSetFog(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED bool enable, MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float start, MAYBE_UNUSED float end) {}

/* ===================================================================== */
/* Surfaces                                                               */
/*                                                                        */
/* The PVR can only render a whole scene to texture, which doesn't map    */
/* onto GameMaker's interleaved surface drawing. Surfaces are tracked so  */
/* game logic keeps working, but drawing into them is skipped and drawing */
/* them is a no-op. Undertale only uses one surface (a transition effect).*/
/* ===================================================================== */

static int32_t dcCreateSurface(Renderer* renderer, int32_t width, int32_t height) {
    DcRenderer* dc = (DcRenderer*) renderer;
    int32_t id = dc->surfaceCount++;
    dc->surfaceW = (int32_t*) safeRealloc(dc->surfaceW, dc->surfaceCount * sizeof(int32_t));
    dc->surfaceH = (int32_t*) safeRealloc(dc->surfaceH, dc->surfaceCount * sizeof(int32_t));
    dc->surfaceAlive = (bool*) safeRealloc(dc->surfaceAlive, dc->surfaceCount * sizeof(bool));
    dc->surfaceW[id] = width; dc->surfaceH[id] = height; dc->surfaceAlive[id] = true;
    return id;
}
static bool dcSurfaceExists(Renderer* renderer, int32_t id) {
    DcRenderer* dc = (DcRenderer*) renderer;
    if (id == APPLICATION_SURFACE_ID) return true;
    return id >= 0 && id < dc->surfaceCount && dc->surfaceAlive[id];
}
static bool dcSetRenderTarget(Renderer* renderer, int32_t id, MAYBE_UNUSED bool implicitApplicationSurface) {
    DcRenderer* dc = (DcRenderer*) renderer;
    dc->currentTarget = id;
    dc->drawingToSurface = !(id == APPLICATION_SURFACE_ID || id == RENDER_TARGET_HOST_FRAMEBUFFER);
    return true;
}
static int32_t dcEnsureApplicationSurface(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t w, MAYBE_UNUSED int32_t h) { return APPLICATION_SURFACE_ID; }
static float dcGetSurfaceWidth(Renderer* renderer, int32_t id) {
    DcRenderer* dc = (DcRenderer*) renderer;
    if (id >= 0 && id < dc->surfaceCount) return (float) dc->surfaceW[id];
    return (float) dc->gameW;
}
static float dcGetSurfaceHeight(Renderer* renderer, int32_t id) {
    DcRenderer* dc = (DcRenderer*) renderer;
    if (id >= 0 && id < dc->surfaceCount) return (float) dc->surfaceH[id];
    return (float) dc->gameH;
}
static void dcDrawSurface(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t id, MAYBE_UNUSED int32_t sl, MAYBE_UNUSED int32_t st, MAYBE_UNUSED int32_t sw, MAYBE_UNUSED int32_t sh, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xs, MAYBE_UNUSED float ys, MAYBE_UNUSED float a, MAYBE_UNUSED uint32_t c, MAYBE_UNUSED float al) {}
static void dcDrawSurfaceColor(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t id, MAYBE_UNUSED int32_t sl, MAYBE_UNUSED int32_t st, MAYBE_UNUSED int32_t sw, MAYBE_UNUSED int32_t sh, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xs, MAYBE_UNUSED float ys, MAYBE_UNUSED float a, MAYBE_UNUSED uint32_t c1, MAYBE_UNUSED uint32_t c2, MAYBE_UNUSED uint32_t c3, MAYBE_UNUSED uint32_t c4, MAYBE_UNUSED float al) {}
static void dcDrawSurfaceTiled(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t id, MAYBE_UNUSED float x, MAYBE_UNUSED float y, MAYBE_UNUSED float xs, MAYBE_UNUSED float ys, MAYBE_UNUSED float rw, MAYBE_UNUSED float rh, MAYBE_UNUSED uint32_t c, MAYBE_UNUSED float a) {}
static void dcSurfaceResize(Renderer* renderer, int32_t id, int32_t w, int32_t h) {
    DcRenderer* dc = (DcRenderer*) renderer;
    if (id >= 0 && id < dc->surfaceCount) { dc->surfaceW[id] = w; dc->surfaceH[id] = h; }
}
static void dcSurfaceFree(Renderer* renderer, int32_t id) {
    DcRenderer* dc = (DcRenderer*) renderer;
    if (id >= 0 && id < dc->surfaceCount) dc->surfaceAlive[id] = false;
    if (dc->currentTarget == id) dcSetRenderTarget(renderer, APPLICATION_SURFACE_ID, true);
}
static void dcSurfaceCopy(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t d, MAYBE_UNUSED int32_t dx, MAYBE_UNUSED int32_t dy, MAYBE_UNUSED int32_t s, MAYBE_UNUSED int32_t sx, MAYBE_UNUSED int32_t sy, MAYBE_UNUSED int32_t sw, MAYBE_UNUSED int32_t sh, MAYBE_UNUSED bool part) {}
static bool dcSurfaceGetPixels(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t id, MAYBE_UNUSED uint8_t* out) { return false; }

/* ===================================================================== */
/* Texture handles (TPAG indices)                                         */
/* ===================================================================== */

static uint32_t dcSpriteGetTexture(MAYBE_UNUSED Renderer* renderer, int32_t tpagIndex) { return (uint32_t) tpagIndex; }
static uint32_t dcSurfaceGetTexture(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID) { return 0xFFFFFFFFu; }
static float dcTextureGetTexelWidth(Renderer* renderer, uint32_t tex) {
    DataWin* dw = renderer->dataWin;
    if (tex < dw->tpag.count && dw->tpag.items[tex].sourceWidth) return 1.0f / (float) dw->tpag.items[tex].sourceWidth;
    return 1.0f;
}
static float dcTextureGetTexelHeight(Renderer* renderer, uint32_t tex) {
    DataWin* dw = renderer->dataWin;
    if (tex < dw->tpag.count && dw->tpag.items[tex].sourceHeight) return 1.0f / (float) dw->tpag.items[tex].sourceHeight;
    return 1.0f;
}
static bool dcTextureGetUVs(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED uint32_t tex, float* outUVs) {
    /* Primitive UVs are relative to the TPAG item (0..1), see dcPrimitiveEnd */
    outUVs[0] = 0; outUVs[1] = 0; outUVs[2] = 1; outUVs[3] = 1;
    return true;
}
static void dcTextureSetStage(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t slot, MAYBE_UNUSED uint32_t tex) {}
static void dcGpuSetShader(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t s) {}
static void dcGpuResetShader(MAYBE_UNUSED Renderer* r) {}
static int32_t dcShaderGetUniform(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t s, MAYBE_UNUSED char* u) { return -1; }
static int32_t dcShaderGetSamplerIndex(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t s, MAYBE_UNUSED char* u) { return -1; }
static void dcShaderSetUniformF(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t h, MAYBE_UNUSED int32_t c, MAYBE_UNUSED float a, MAYBE_UNUSED float b, MAYBE_UNUSED float cc, MAYBE_UNUSED float d) {}
static void dcShaderSetUniformFArray(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t h, MAYBE_UNUSED float* v, MAYBE_UNUSED uint32_t c) {}
static void dcShaderSetUniformI(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t h, MAYBE_UNUSED int32_t c, MAYBE_UNUSED int32_t a, MAYBE_UNUSED int32_t b, MAYBE_UNUSED int32_t cc, MAYBE_UNUSED int32_t d) {}
static bool dcShaderIsCompiled(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t s) { return false; }
static bool dcShadersSupported(void) { return false; }
static void dcSetMatrix(Renderer* renderer, int32_t type, Matrix4f m) { if (type >= 0 && type < MATRICES_MAX) renderer->gmlMatrices[type] = m; }
static void dcSetDepthState(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED bool t, MAYBE_UNUSED bool w, MAYBE_UNUSED bool c) {}
static void dcClearDepth(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED float d) {}
static bool dcSurfaceSetPixels(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t id, MAYBE_UNUSED const uint8_t* rgba) { return false; }
static void dcSurfaceUploadPixels(MAYBE_UNUSED Renderer* r, MAYBE_UNUSED int32_t id, MAYBE_UNUSED int32_t w, MAYBE_UNUSED int32_t h, MAYBE_UNUSED const uint8_t* rgba) {}

static RendererVtable dcVtable;

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

Renderer* DcRenderer_create(const char* texturesPath) {
    DcRenderer* dc = (DcRenderer*) safeCalloc(1, sizeof(DcRenderer));
    dc->texFile = FILEHND_INVALID;
    if (!openTextures(dc, texturesPath)) {
        free(dc);
        return nullptr;
    }
    dc->origTpagCount = dc->texHdr.itemCount;
    /* 8x8 white texture */
    dc->whiteTex = pvr_mem_malloc(8 * 8 * 2);
    volatile uint16_t* w = (volatile uint16_t*) dc->whiteTex;
    for (int i = 0; i < 64; i++) w[i] = 0xFFFF;

    RendererVtable* v = &dcVtable;
    memset(v, 0, sizeof *v);
    v->init = dcInit; v->destroy = dcDestroy;
    v->beginFrame = dcBeginFrame; v->endFrameInit = dcEndFrameInit; v->endFrameEnd = dcEndFrameEnd;
    v->beginView = dcBeginView; v->endView = dcEndView; v->applyProjection = dcApplyProjection;
    v->beginGUI = dcBeginGUI; v->setGuiProjection = dcSetGuiProjection; v->endGUI = dcEndGUI;
    v->drawSprite = dcDrawSprite; v->drawSpritePart = dcDrawSpritePart; v->drawSpritePartColor = dcDrawSpritePartColor;
    v->drawSpritePos = dcDrawSpritePos;
    v->drawRectangle = dcDrawRectangle; v->drawRectangleColor = dcDrawRectangleColor;
    v->drawLine = dcDrawLine; v->drawTriangle = dcDrawTriangle; v->drawLineColor = dcDrawLineColor;
    v->drawText = dcDrawText; v->drawTextColor = dcDrawTextColor; v->drawTextUI = dcDrawTextUI;
    v->primitiveBegin = dcPrimitiveBegin; v->primitiveBeginTexture = dcPrimitiveBeginTexture;
    v->primitiveEnd = dcPrimitiveEnd; v->drawVertex = dcDrawVertex; v->drawVertexBuffer = dcDrawVertexBuffer;
    v->flush = dcFlush; v->clearScreen = dcClearScreen;
    v->createSpriteFromSurface = dcCreateSpriteFromSurface; v->deleteSprite = dcDeleteSprite;
    v->gpuGetBlendFactors = dcGpuGetBlendFactors; v->gpuGetBlendMode = dcGpuGetBlendMode;
    v->gpuSetBlendMode = dcGpuSetBlendMode; v->gpuSetBlendModeExt = dcGpuSetBlendModeExt;
    v->gpuSetBlendEnable = dcGpuSetBlendEnable; v->gpuSetTexFilter = dcGpuSetTexFilter;
    v->gpuSetAlphaTestEnable = dcGpuSetAlphaTestEnable; v->gpuGetAlphaTestEnable = dcGpuGetAlphaTestEnable;
    v->gpuSetAlphaTestRef = dcGpuSetAlphaTestRef; v->gpuSetColorWriteEnable = dcGpuSetColorWriteEnable;
    v->gpuGetColorWriteEnable = dcGpuGetColorWriteEnable; v->gpuGetBlendEnable = dcGpuGetBlendEnable;
    v->gpuSetFog = dcGpuSetFog;
    v->drawTile = nullptr; /* generic implementation in renderer.h uses drawSpritePart */
    v->drawSpriteTiled = dcDrawSpriteTiled;
    v->createSurface = dcCreateSurface; v->surfaceExists = dcSurfaceExists; v->setRenderTarget = dcSetRenderTarget;
    v->ensureApplicationSurface = dcEnsureApplicationSurface;
    v->getSurfaceWidth = dcGetSurfaceWidth; v->getSurfaceHeight = dcGetSurfaceHeight;
    v->drawSurface = dcDrawSurface; v->drawSurfaceColor = dcDrawSurfaceColor; v->drawSurfaceTiled = dcDrawSurfaceTiled;
    v->surfaceResize = dcSurfaceResize; v->surfaceFree = dcSurfaceFree; v->surfaceCopy = dcSurfaceCopy;
    v->surfaceGetPixels = dcSurfaceGetPixels; v->surfaceSetPixels = dcSurfaceSetPixels; v->surfaceUploadPixels = dcSurfaceUploadPixels;
    v->drawTiledPart = nullptr;
    v->gpuSetShader = dcGpuSetShader; v->gpuResetShader = dcGpuResetShader;
    v->shaderGetUniform = dcShaderGetUniform; v->shaderGetSamplerIndex = dcShaderGetSamplerIndex;
    v->shaderSetUniformF = dcShaderSetUniformF; v->shaderSetUniformFArray = dcShaderSetUniformFArray; v->shaderSetUniformI = dcShaderSetUniformI;
    v->spriteGetTexture = dcSpriteGetTexture; v->surfaceGetTexture = dcSurfaceGetTexture;
    v->textureGetTexelWidth = dcTextureGetTexelWidth; v->textureGetTexelHeight = dcTextureGetTexelHeight;
    v->textureGetUVs = dcTextureGetUVs; v->textureSetStage = dcTextureSetStage;
    v->shaderIsCompiled = dcShaderIsCompiled; v->shadersSupported = dcShadersSupported;
    v->setMatrix = dcSetMatrix; v->setDepthState = dcSetDepthState; v->clearDepth = dcClearDepth;
    dc->base.vtable = v;
    dc->winScaleX = dc->winScaleY = 1.0f;
    dc->scaleX = dc->scaleY = 1.0f;
    return &dc->base;
}

void DcRenderer_beginScene(Renderer* renderer) {
    DcRenderer* dc = (DcRenderer*) renderer;
    if (g_texProfEvery < 0) { const char* e = getenv("BS_TEX_PROFILE"); g_texProfEvery = e ? atoi(e) : 0; }
    texProfReport(dc);
    dc->frame++;
    g_renderSynced = false;
    freeOldSnapshots(dc);
    if (g_fbReadable == 0) { g_recCur ^= 1; g_recLen[g_recCur] = 0; }
    dc->stats.pageLoadsFrame = 0;
    dc->stats.polysFrame = 0;
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);
    dc->curPage = -2;
    dc->inScene = true;
}

/* A "null" frame runs the game's draw events (Undertale keeps game logic in some of them) but
   renders nothing: no texture uploads, nothing sent to the TA, the previous picture stays on
   screen. Used to catch up when the game falls behind real time. */
void DcRenderer_beginNullScene(Renderer* renderer) {
    DcRenderer* dc = (DcRenderer*) renderer;
    dc->nullFrame = true;
    dc->inScene = false;
    dc->stats.polysFrame = 0;
}

/* Replays a recorded frame into a texture: screen position p becomes (p - origin) * scale.
   Headers that sample the rolling capture are skipped when rendering into it. */
static void replayInto(pvr_ptr_t tex, uint32_t texW, uint32_t renderW, uint32_t renderH, int rec, float scale, float ox, float oy, bool skipRoll) {
    if (renderW < 8 || renderH < 8) return;
    g_replaying = true;
    pvr_wait_ready();
    if (pvr_scene_begin_rtt(tex, renderW, renderH, texW) < 0) { g_replaying = false; return; }
    pvr_list_begin(PVR_LIST_TR_POLY);
    bool skipping = false;
    const uint8_t* cmd = g_rec[rec];
    for (uint32_t i = 0; i < g_recLen[rec]; i++, cmd += 32) {
        uint8_t kind = g_recKind[rec][i];
        if (kind != 0) {
            skipping = skipRoll && kind == 2;
            if (!skipping) pvr_prim((void*) cmd, 32);
            continue;
        }
        if (skipping) continue;
        pvr_vertex_t v;
        memcpy(&v, cmd, 32);
        v.x = (v.x - ox) * scale;
        v.y = (v.y - oy) * scale;
        pvr_prim(&v, 32);
    }
    pvr_list_finish();
    pvr_scene_finish();
    g_replaying = false;
}

/* After the frame's scene: fill captures made during this frame from the previous frame's
   recording, then refresh the rolling capture while captures are being made. */
static void captureWork(DcRenderer* dc) {
    int prev = g_recCur ^ 1, cur = g_recCur;
    for (int32_t k = 0; k < (int32_t) arrlen(g_snaps); k++) {
        DcSnapshot* sn = &g_snaps[k];
        if (!sn->used || !sn->rtt || sn->freeAtFrame) continue;
        if (sn->live) {
            /* outlived its frame: give it its own copy before the rolling capture changes */
            uint32_t sw = (uint32_t) (sn->rectW + 1) / 2, sh = (uint32_t) (sn->rectH + 1) / 2;
            if (sw > 512) sw = 512;
            if (sh > 256) sh = 256;
            uint32_t texW = nextPow2(sw), texH = nextPow2(sh);
            pvr_ptr_t mem = nullptr;
            if (pvr_mem_available() >= texW * texH * 2 + 4096) mem = pvr_mem_malloc(texW * texH * 2);
            if (!mem) continue;   /* keeps showing the rolling capture */
            sn->vram = mem; sn->texW = (uint16_t) texW; sn->texH = (uint16_t) texH;
            sn->offU = sn->offV = 0; sn->live = false; sn->ownsVram = true; sn->pending = true;
        }
        if (sn->pending) {
            uint32_t rw = (uint32_t) (sn->rectW + 1) / 2, rh = (uint32_t) (sn->rectH + 1) / 2;
            if (rw > sn->texW) rw = sn->texW;
            if (rh > sn->texH) rh = sn->texH;
            replayInto(sn->vram, sn->texW, rw, rh, prev, 0.5f, (float) sn->rectX, (float) sn->rectY, false);
            sn->pending = false;
        }
    }
    if (dc->frame - g_lastSnapReq < 60) {
        if (!g_rollTex) {
            poolInit();
            g_rollTex = g_poolBase ? poolAllocEvicting(dc, ROLL_W * ROLL_H * 2, -2) : nullptr;
        }
        if (g_rollTex) {
            replayInto(g_rollTex, ROLL_W, 320, 240, cur, 0.5f, 0.0f, 0.0f, true);
            g_rollValidFrame = dc->frame;
        }
    } else if (g_rollTex) {
        bool inUse = false;
        for (int32_t k = 0; k < (int32_t) arrlen(g_snaps); k++) if (g_snaps[k].used && g_snaps[k].vram == g_rollTex) inUse = true;
        if (!inUse) { poolFreePtr(g_rollTex); g_rollTex = nullptr; g_rollValidFrame = 0; }
    }
}

/* Boot probe: render a frame of a known colour and read the displayed framebuffer back. Real
   hardware (and emulators that emulate the framebuffer) return it; emulators that keep the
   picture on the host GPU don't, and screen captures then use render-to-texture.
   BS_FB_READBACK=0/1 overrides the result (testing). */
void DcRenderer_probeFramebuffer(Renderer* renderer) {
    DcRenderer* dc = (DcRenderer*) renderer;
    const char* force = getenv("BS_FB_READBACK");
    if (force) g_fbReadable = atoi(force) ? 1 : 0;
    else {
        pvr_set_bg_color(8.0f / 255.0f, 4.0f / 255.0f, 8.0f / 255.0f);   /* RGB565 0x0821 */
        for (int i = 0; i < 3; i++) {
            pvr_wait_ready();
            pvr_scene_begin();
            pvr_list_begin(PVR_LIST_TR_POLY);
            pvr_list_finish();
            pvr_scene_finish();
        }
        pvr_wait_ready();
        thd_sleep(50);
        uint32_t off = PVR_GET(PVR_FB_ADDR) & 0x007FFFFFu;
        const volatile uint16_t* fb = (const volatile uint16_t*) (PVR_RAM_BASE + off);
        uint16_t a = fb[240 * 640 + 320], b = fb[100 * 640 + 100];
        g_fbReadable = (a == 0x0821 && b == 0x0821) ? 1 : 0;
        pvr_set_bg_color(0.0f, 0.0f, 0.0f);
        logInfo("DC: framebuffer read-back %s (probe read %04x %04x)\n", g_fbReadable ? "works" : "unavailable: screen captures use render-to-texture", (unsigned) a, (unsigned) b);
    }
    if (g_fbReadable == 0) {
        for (int i = 0; i < 2; i++) {
            g_rec[i] = (uint8_t*) memalign(32, REC_CAP);
            g_recKind[i] = (uint8_t*) malloc(REC_CAP / 32);
            if (!g_rec[i] || !g_recKind[i]) { logWarn("DC: no RAM for capture recording\n"); }
        }
    }
    (void) dc;
}

void DcRenderer_endScene(Renderer* renderer) {
    DcRenderer* dc = (DcRenderer*) renderer;
    if (dc->nullFrame) { dc->nullFrame = false; return; }
    dc->inScene = false;
    pvr_list_finish();
    pvr_scene_finish();
    if (g_fbReadable == 0 && g_rec[0] && g_rec[1]) captureWork(dc);
    dc->stats.vramFree = g_poolFree;
}

void DcRenderer_screenRect(Renderer* renderer, float x0, float y0, float x1, float y1, uint32_t argb) {
    DcRenderer* dc = (DcRenderer*) renderer;
    float sx = dc->scaleX, sy = dc->scaleY, ox = dc->offX, oy = dc->offY;
    bool rot = dc->viewRotated;
    dc->scaleX = dc->scaleY = 1.0f; dc->offX = dc->offY = 0.0f; dc->viewRotated = false;
    float wx[4] = { x0, x1, x1, x0 }, wy[4] = { y0, y0, y1, y1 };
    uint32_t cs[4] = { argb, argb, argb, argb };
    solidQuad(dc, wx, wy, cs);
    dc->scaleX = sx; dc->scaleY = sy; dc->offX = ox; dc->offY = oy; dc->viewRotated = rot;
}

const DcRendererStats* DcRenderer_stats(Renderer* renderer) { return &((DcRenderer*) renderer)->stats; }
