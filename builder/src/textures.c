/* Converts the GameMaker texture pages of a data.win into Dreamcast-ready
 * TEXTURES.BIN (see common/utdc_formats.h).
 *
 * GameMaker packs sprites into 2048x2048 PNG pages. The PowerVR2 can't use
 * textures above 1024x1024 and there is only ~5 MB of free VRAM, so every
 * TPAG item is cut out and repacked into small power-of-two pages that the
 * runner can stream in and evict on demand.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "build.h"
#include "datawin.h"
#include "../../common/utdc_formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../third_party/stb/stb_image.h"
#include "../third_party/lz4/lz4hc.h"

#define PAGE_SIZE 256  /* small pages: a frame only pays VRAM for the parts of an atlas it uses */
#define MAX_TEX   1024
#define GAP       1

typedef struct { int x, y, w; } SkyNode;

typedef struct {
    uint8_t* rgba;           /* PAGE_SIZE*PAGE_SIZE*4, or custom for big items */
    int w, h;
    int usedW, usedH;
    int soft;                /* page holds soft-alpha items (ARGB4444) */
    SkyNode nodes[PAGE_SIZE + 2];
    int nodeCount;
    int itemCount;
} OpenPage;

typedef struct {
    uint8_t* comp; uint32_t compSize;
    uint16_t w, h; uint8_t fmt;
} DonePage;

typedef struct {
    DonePage* pages; int count, cap;
    UtdcTexItem* items;
    uint32_t largestRaw, largestComp;
    uint64_t rawTotal, compTotal;
} TexOut;

static int pow2ceil(int v) { int p = 8; while (p < v) p <<= 1; return p; }

static void sky_reset(OpenPage* p) {
    p->nodeCount = 1;
    p->nodes[0].x = 0; p->nodes[0].y = 0; p->nodes[0].w = p->w;
    p->usedW = p->usedH = 0; p->itemCount = 0;
}

/* Skyline bottom-left fit; returns 1 and sets *ox,*oy on success */
static int sky_fit(OpenPage* p, int w, int h, int* ox, int* oy) {
    int bestY = 1 << 30, bestX = 0, bestIdx = -1, bestW = 1 << 30;
    for (int i = 0; i < p->nodeCount; i++) {
        int x = p->nodes[i].x;
        if (x + w > p->w) break;
        int y = 0, remaining = w, j = i;
        while (remaining > 0) {
            if (j >= p->nodeCount) { y = 1 << 30; break; }
            if (p->nodes[j].y > y) y = p->nodes[j].y;
            remaining -= p->nodes[j].w;
            j++;
        }
        if (y + h > p->h) continue;
        if (y < bestY || (y == bestY && p->nodes[i].w < bestW)) {
            bestY = y; bestX = x; bestIdx = i; bestW = p->nodes[i].w;
        }
    }
    if (bestIdx < 0) return 0;
    /* insert new node */
    SkyNode nn = { bestX, bestY + h, w };
    memmove(&p->nodes[bestIdx + 1], &p->nodes[bestIdx], (size_t)(p->nodeCount - bestIdx) * sizeof(SkyNode));
    p->nodes[bestIdx] = nn;
    p->nodeCount++;
    for (int i = bestIdx + 1; i < p->nodeCount; i++) {
        SkyNode* prev = &p->nodes[i - 1];
        SkyNode* cur = &p->nodes[i];
        if (cur->x < prev->x + prev->w) {
            int shrink = prev->x + prev->w - cur->x;
            cur->x += shrink; cur->w -= shrink;
            if (cur->w <= 0) {
                memmove(&p->nodes[i], &p->nodes[i + 1], (size_t)(p->nodeCount - i - 1) * sizeof(SkyNode));
                p->nodeCount--; i--;
            } else break;
        } else break;
    }
    for (int i = 0; i + 1 < p->nodeCount; i++) {
        if (p->nodes[i].y == p->nodes[i + 1].y) {
            p->nodes[i].w += p->nodes[i + 1].w;
            memmove(&p->nodes[i + 1], &p->nodes[i + 2], (size_t)(p->nodeCount - i - 2) * sizeof(SkyNode));
            p->nodeCount--; i--;
        }
    }
    *ox = bestX; *oy = bestY;
    if (bestX + w > p->usedW) p->usedW = bestX + w;
    if (bestY + h > p->usedH) p->usedH = bestY + h;
    return 1;
}

static uint32_t twid(uint32_t v) {
    uint32_t r = 0;
    for (int b = 0; b < 11; b++) r |= ((v >> b) & 1u) << (2 * b);
    return r;
}

static uint16_t conv_px(const uint8_t* c, int soft) {
    uint8_t r = c[0], g = c[1], b = c[2], a = c[3];
    if (soft) {
        return (uint16_t)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
    }
    if (a < 128) return 0;
    return (uint16_t)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}

static int finish_page(TexOut* out, OpenPage* p, BuildContext* ctx) {
    if (p->itemCount == 0) return 0;
    /* shrink to used area */
    int w = pow2ceil(p->usedW), h = pow2ceil(p->usedH);
    if (w > p->w) w = p->w;
    if (h > p->h) h = p->h;
    uint16_t* tw = (uint16_t*)calloc((size_t)w * h, 2);
    if (!tw) return -1;
    int mn = w < h ? w : h, mask = mn - 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint16_t px = conv_px(p->rgba + ((size_t)y * p->w + x) * 4, p->soft);
            uint32_t idx = (twid((uint32_t)(y & mask)) | (twid((uint32_t)(x & mask)) << 1)) + (uint32_t)((x / mn + y / mn) * mn * mn);
            tw[idx] = px;
        }
    }
    /* LZ4-HC in independent 32 KB blocks: decodes ~10x faster than zlib on the SH-4 */
    uint32_t rawBytes = (uint32_t) w * h * 2;
    uint32_t maxOut = 0;
    for (uint32_t o = 0; o < rawBytes; o += UTDC_TEX_BLOCK_RAW) {
        uint32_t n = rawBytes - o < UTDC_TEX_BLOCK_RAW ? rawBytes - o : UTDC_TEX_BLOCK_RAW;
        maxOut += (uint32_t) sizeof(UtdcTexBlock) + (uint32_t) LZ4_compressBound((int) n);
    }
    uint8_t* comp = (uint8_t*)malloc(maxOut);
    if (!comp) { free(tw); return -1; }
    uint32_t cl = 0;
    for (uint32_t o = 0; o < rawBytes; o += UTDC_TEX_BLOCK_RAW) {
        uint32_t n = rawBytes - o < UTDC_TEX_BLOCK_RAW ? rawBytes - o : UTDC_TEX_BLOCK_RAW;
        int c = LZ4_compress_HC((const char*) tw + o, (char*) comp + cl + sizeof(UtdcTexBlock), (int) n,
                                LZ4_compressBound((int) n), LZ4HC_CLEVEL_MAX);
        if (c <= 0) { free(tw); free(comp); return -1; }
        UtdcTexBlock bh = { (uint32_t) c, n };
        memcpy(comp + cl, &bh, sizeof bh);
        cl += (uint32_t) sizeof(UtdcTexBlock) + (uint32_t) c;
    }
    if (out->count == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 64;
        out->pages = (DonePage*)realloc(out->pages, (size_t)out->cap * sizeof(DonePage));
    }
    DonePage* d = &out->pages[out->count++];
    d->comp = (uint8_t*)realloc(comp, cl); d->compSize = (uint32_t)cl;
    d->w = (uint16_t)w; d->h = (uint16_t)h; d->fmt = p->soft ? UTDC_TEXFMT_ARGB4444 : UTDC_TEXFMT_ARGB1555;
    uint32_t raw = (uint32_t)w * h * 2;
    if (raw > out->largestRaw) out->largestRaw = raw;
    if (d->compSize > out->largestComp) out->largestComp = d->compSize;
    out->rawTotal += raw; out->compTotal += cl;
    memset(p->rgba, 0, (size_t)p->w * p->h * 4);
    sky_reset(p);
    (void)ctx;
    return 0;
}

typedef struct { uint32_t tpag; int dupOf; int soft; } ItemInfo;

static const DataWinFile* item_cmp_dw;
static int cmp_items(const void* a, const void* b) {
    const DwTpag* ta = &item_cmp_dw->tpag[*(const uint32_t*)a];
    const DwTpag* tb = &item_cmp_dw->tpag[*(const uint32_t*)b];
    if (ta->srcH != tb->srcH) return tb->srcH - ta->srcH;
    if (ta->srcW != tb->srcW) return tb->srcW - ta->srcW;
    return (int)(*(const uint32_t*)a) - (int)(*(const uint32_t*)b);
}

int build_textures(BuildContext* ctx, const DataWinFile* dw, const char* outPath) {
    TexOut out = {0};
    out.items = (UtdcTexItem*)calloc(dw->tpagCount ? dw->tpagCount : 1, sizeof(UtdcTexItem));
    for (uint32_t i = 0; i < dw->tpagCount; i++) out.items[i].page = UTDC_TEX_NO_PAGE;

    OpenPage open[2];
    for (int s = 0; s < 2; s++) {
        memset(&open[s], 0, sizeof(OpenPage));
        open[s].w = open[s].h = PAGE_SIZE; open[s].soft = s;
        open[s].rgba = (uint8_t*)calloc(PAGE_SIZE * PAGE_SIZE, 4);
        sky_reset(&open[s]);
    }
    /* page index that each open page will get once finished */
    uint32_t* order = (uint32_t*)malloc((dw->tpagCount + 1) * sizeof(uint32_t));
    /* Pending assignment: items placed into an open page get their final page id when it closes */
    int32_t* pendingSlot = (int32_t*)malloc((dw->tpagCount + 1) * sizeof(int32_t));
    for (uint32_t i = 0; i < dw->tpagCount; i++) pendingSlot[i] = -1;

    for (uint32_t pg = 0; pg < dw->texCount; pg++) {
        if (build_cancelled(ctx)) goto fail;
        build_progress(ctx, 0.05 + 0.35 * pg / (dw->texCount ? dw->texCount : 1), "Converting texture page %u/%u", pg + 1, dw->texCount);
        if (!dw->tex[pg].offset) continue;
        int pw, ph, comp;
        uint8_t* img = stbi_load_from_memory(dw->data + dw->tex[pg].offset, (int)dw->tex[pg].size, &pw, &ph, &comp, 4);
        if (!img) { build_log(ctx, "warning: texture page %u could not be decoded", pg); continue; }

        uint32_t n = 0;
        for (uint32_t i = 0; i < dw->tpagCount; i++)
            if (dw->tpag[i].page == (int16_t)pg && dw->tpag[i].srcW && dw->tpag[i].srcH) order[n++] = i;
        item_cmp_dw = dw;
        qsort(order, n, sizeof(uint32_t), cmp_items);

        for (uint32_t k = 0; k < n; k++) {
            uint32_t ti = order[k];
            const DwTpag* t = &dw->tpag[ti];
            /* dedupe identical regions */
            int dup = -1;
            for (uint32_t j = 0; j < k; j++) {
                const DwTpag* o = &dw->tpag[order[j]];
                if (o->srcX == t->srcX && o->srcY == t->srcY && o->srcW == t->srcW && o->srcH == t->srcH) { dup = (int)order[j]; break; }
            }
            if (dup >= 0) {
                out.items[ti] = out.items[dup];
                pendingSlot[ti] = pendingSlot[dup];
                continue;
            }
            int sx = t->srcX, sy = t->srcY, sw = t->srcW, sh = t->srcH;
            if (sx + sw > pw) sw = pw - sx;
            if (sy + sh > ph) sh = ph - sy;
            if (sw <= 0 || sh <= 0) continue;
            int soft = 0;
            for (int y = 0; y < sh && !soft; y++)
                for (int x = 0; x < sw; x++) {
                    uint8_t a = img[((size_t)(sy + y) * pw + sx + x) * 4 + 3];
                    if (a != 0 && a != 255) { soft = 1; break; }
                }
            int dw_ = sw, dh_ = sh;
            if (dw_ > MAX_TEX) dw_ = MAX_TEX;
            if (dh_ > MAX_TEX) dh_ = MAX_TEX;

            OpenPage* target;
            OpenPage big;
            int px, py;
            int isBig = (dw_ + GAP > PAGE_SIZE || dh_ + GAP > PAGE_SIZE);
            if (isBig) {
                memset(&big, 0, sizeof(big));
                big.w = pow2ceil(dw_); big.h = pow2ceil(dh_); big.soft = soft;
                big.rgba = (uint8_t*)calloc((size_t)big.w * big.h, 4);
                sky_reset(&big);
                px = py = 0; big.usedW = dw_; big.usedH = dh_;
                target = &big;
            } else {
                target = &open[soft];
                if (!sky_fit(target, dw_ + GAP, dh_ + GAP, &px, &py)) {
                    /* close page and assign ids */
                    int id = out.count;
                    if (finish_page(&out, target, ctx)) goto fail;
                    for (uint32_t q = 0; q < dw->tpagCount; q++) if (pendingSlot[q] == soft) { out.items[q].page = (uint16_t)id; pendingSlot[q] = -1; }
                    if (!sky_fit(target, dw_ + GAP, dh_ + GAP, &px, &py)) { build_log(ctx, "warning: TPAG %u too big", ti); continue; }
                }
            }
            /* blit (with nearest downscale if needed) */
            for (int y = 0; y < dh_; y++) {
                int srcy = sy + (int)((int64_t)y * sh / dh_);
                for (int x = 0; x < dw_; x++) {
                    int srcx = sx + (int)((int64_t)x * sw / dw_);
                    memcpy(target->rgba + ((size_t)(py + y) * target->w + px + x) * 4, img + ((size_t)srcy * pw + srcx) * 4, 4);
                }
            }
            target->itemCount++;
            out.items[ti].x = (uint16_t)px; out.items[ti].y = (uint16_t)py;
            out.items[ti].w = (uint16_t)dw_; out.items[ti].h = (uint16_t)dh_;
            if (isBig) {
                int id = out.count;
                if (finish_page(&out, &big, ctx)) goto fail;
                free(big.rgba);
                out.items[ti].page = (uint16_t)id;
            } else {
                pendingSlot[ti] = soft;
            }
        }
        stbi_image_free(img);
    }
    for (int s = 0; s < 2; s++) {
        int id = out.count;
        if (finish_page(&out, &open[s], ctx)) goto fail;
        for (uint32_t q = 0; q < dw->tpagCount; q++) if (pendingSlot[q] == s) { out.items[q].page = (uint16_t)id; pendingSlot[q] = -1; }
    }

    {
        FILE* f = fopen(outPath, "wb");
        if (!f) { build_log(ctx, "error: cannot write %s", outPath); goto fail; }
        UtdcTexHeader hdr = {0};
        hdr.magic = UTDC_TEX_MAGIC; hdr.version = UTDC_FORMAT_VERSION;
        hdr.pageCount = (uint32_t)out.count; hdr.itemCount = dw->tpagCount;
        hdr.largestPageBytes = out.largestRaw; hdr.largestCompBytes = out.largestComp;
        uint32_t off = (uint32_t)(sizeof(hdr) + out.count * sizeof(UtdcTexPage) + dw->tpagCount * sizeof(UtdcTexItem));
        off = (off + 2047) & ~2047u;   /* pages start on CD sector boundaries: one DMA read per page */
        fwrite(&hdr, sizeof(hdr), 1, f);
        for (int i = 0; i < out.count; i++) {
            UtdcTexPage pgd = {0};
            pgd.offset = off; pgd.compSize = out.pages[i].compSize;
            pgd.width = out.pages[i].w; pgd.height = out.pages[i].h; pgd.format = out.pages[i].fmt;
            fwrite(&pgd, sizeof(pgd), 1, f);
            off = (off + out.pages[i].compSize + 2047) & ~2047u;
        }
        fwrite(out.items, sizeof(UtdcTexItem), dw->tpagCount, f);
        long pos = ftell(f);
        static const uint8_t zeros[2048] = {0};
        fwrite(zeros, 1, (size_t)(((pos + 2047) & ~2047) - pos), f);
        for (int i = 0; i < out.count; i++) {
            fwrite(out.pages[i].comp, 1, out.pages[i].compSize, f);
            pos = ftell(f);
            fwrite(zeros, 1, (size_t)(((pos + 2047) & ~2047) - pos), f);
        }
        int bad = ferror(f);
        fclose(f);
        if (bad) { build_log(ctx, "error: write failed for %s", outPath); goto fail; }
    }
    build_log(ctx, "Textures: %d Dreamcast pages, %.1f MB raw -> %.1f MB compressed", out.count, out.rawTotal / 1048576.0, out.compTotal / 1048576.0);
    ctx->stats.texPages = out.count;

    for (int i = 0; i < out.count; i++) free(out.pages[i].comp);
    free(out.pages); free(out.items); free(order); free(pendingSlot);
    free(open[0].rgba); free(open[1].rgba);
    return 0;
fail:
    for (int i = 0; i < out.count; i++) free(out.pages[i].comp);
    free(out.pages); free(out.items); free(order); free(pendingSlot);
    free(open[0].rgba); free(open[1].rgba);
    return -1;
}
