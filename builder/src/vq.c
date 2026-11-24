/* PowerVR2 VQ texture encoder used by the ROM builder.
 *
 * A VQ texture is a 256-entry codebook of 2x2 texel blocks (8 bytes each, texels in twiddled
 * order (0,0) (0,1) (1,0) (1,1)) followed by one index byte per 2x2 block, the index grid itself
 * twiddled. The PVR decodes it while drawing at no cost, so a page needs 1/8 of the VRAM of the
 * same page in 16 bits per texel.
 *
 * Only photographic, many-coloured sprite frames are stored this way (Omega Flowey's body parts
 * and animations); flat pixel art keeps the lossless 16-bit pages.
 *
 * Encoding: identical blocks are merged first (a page that has at most 256 distinct blocks is
 * stored losslessly), then a weighted k-means runs on the distinct blocks. Distances are taken
 * in the 16-bit colours the hardware will actually show, alpha weighted triple so edges stay
 * clean. A fully transparent block always gets an exact transparent codeword.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vq.h"

#define DIM 16   /* 4 texels x RGBA */

/* Blocks are compared in premultiplied colour (what actually reaches the screen), so the colour of
   nearly transparent texels barely matters and opaque detail gets the codewords. Alpha is
   weighted double so outlines stay where they are. */
#define AW 2
static void unpack16(uint16_t p, int soft, int16_t* o) {
    int r, g, b, a;
    if (soft) { r = ((p >> 8) & 15) * 17; g = ((p >> 4) & 15) * 17; b = (p & 15) * 17; a = ((p >> 12) & 15) * 17; }
    else { r = ((p >> 10) & 31) * 255 / 31; g = ((p >> 5) & 31) * 255 / 31; b = (p & 31) * 255 / 31; a = (p & 0x8000) ? 255 : 0; }
    o[0] = (int16_t)(r * a / 255); o[1] = (int16_t)(g * a / 255); o[2] = (int16_t)(b * a / 255); o[3] = (int16_t)(a * AW);
}

static uint16_t pack16(const int32_t* v, int soft) {
    int a = v[3] / AW;
    if (a <= 0) return 0;
    if (a > 255) a = 255;
    int c[3];
    for (int i = 0; i < 3; i++) { int x = v[i] < 0 ? 0 : v[i]; x = (x * 255 + a / 2) / a; c[i] = x > 255 ? 255 : x; }
    if (soft) {
        int a4 = (a + 8) / 17; if (a4 > 15) a4 = 15;
        if (a4 == 0) return 0;
        return (uint16_t)((a4 << 12) | (((c[0] + 8) / 17) << 8) | (((c[1] + 8) / 17) << 4) | ((c[2] + 8) / 17));
    }
    if (a < 128) return 0;
    return (uint16_t)(0x8000 | (((c[0] * 31 + 127) / 255) << 10) | (((c[1] * 31 + 127) / 255) << 5) | ((c[2] * 31 + 127) / 255));
}

static uint32_t twid(uint32_t v) {
    uint32_t r = 0;
    for (int b = 0; b < 11; b++) r |= ((v >> b) & 1u) << (2 * b);
    return r;
}

/* distinct-block table (open addressing on the 4 raw 16-bit texels) */
typedef struct { uint64_t key; uint32_t count; int32_t id; } Slot;

static uint64_t hash64(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33;
    return k;
}

static int32_t dist2(const int16_t* a, const int16_t* b) {
    int32_t s = 0;
    for (int i = 0; i < DIM; i++) { int32_t d = (int32_t)a[i] - (int32_t)b[i]; s += d * d; }
    return s;
}

int vq_encode(const uint16_t* px, int size, int soft, uint8_t* out, double* outErr) {
    int bw = size / 2, nb = bw * bw;
    uint64_t* keys = (uint64_t*)malloc((size_t)nb * sizeof(uint64_t));
    int32_t* blockId = (int32_t*)malloc((size_t)nb * sizeof(int32_t));
    int tabSize = 1; while (tabSize < nb * 2) tabSize <<= 1;
    Slot* tab = (Slot*)calloc((size_t)tabSize, sizeof(Slot));
    uint64_t* ukey = (uint64_t*)malloc((size_t)nb * sizeof(uint64_t));
    uint32_t* ucount = (uint32_t*)malloc((size_t)nb * sizeof(uint32_t));
    if (!keys || !blockId || !tab || !ukey || !ucount) { free(keys); free(blockId); free(tab); free(ukey); free(ucount); return -1; }
    for (int i = 0; i < tabSize; i++) tab[i].id = -1;

    /* gather blocks (row-major block order) and merge duplicates */
    int nu = 0;
    for (int by = 0; by < bw; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int x = bx * 2, y = by * 2;
            uint64_t k = (uint64_t)px[y * size + x] | ((uint64_t)px[(y + 1) * size + x] << 16) |
                         ((uint64_t)px[y * size + x + 1] << 32) | ((uint64_t)px[(y + 1) * size + x + 1] << 48);
            int b = by * bw + bx;
            keys[b] = k;
            uint32_t h = (uint32_t)hash64(k) & (uint32_t)(tabSize - 1);
            while (tab[h].id >= 0 && tab[h].key != k) h = (h + 1) & (uint32_t)(tabSize - 1);
            if (tab[h].id < 0) { tab[h].key = k; tab[h].id = nu; ukey[nu] = k; ucount[nu] = 0; nu++; }
            ucount[tab[h].id]++;
            blockId[b] = tab[h].id;
        }
    }

    int16_t* uvec = (int16_t*)malloc((size_t)nu * DIM * sizeof(int16_t));
    for (int u = 0; u < nu; u++)
        for (int t = 0; t < 4; t++) unpack16((uint16_t)(ukey[u] >> (16 * t)), soft, &uvec[u * DIM + t * 4]);

    uint16_t code[256][4];
    int16_t cvec[256][DIM];
    int32_t* assign = (int32_t*)malloc((size_t)nu * sizeof(int32_t));
    int ncode;
    int transparentU = -1;
    for (int u = 0; u < nu; u++) if (ukey[u] == 0) { transparentU = u; break; }

    if (nu <= 256) {
        /* lossless */
        ncode = nu;
        for (int u = 0; u < nu; u++) {
            for (int t = 0; t < 4; t++) code[u][t] = (uint16_t)(ukey[u] >> (16 * t));
            assign[u] = u;
        }
    } else {
        /* weighted k-means. Code 0 is reserved for the fully transparent block when present. */
        int first = transparentU >= 0 ? 1 : 0;
        int k = 256 - first;
        int32_t (*cen)[DIM] = malloc(sizeof(int32_t[DIM]) * 256);
        /* deterministic farthest-point seeding (k-means++ without randomness): start from the most
           frequent block, then repeatedly take the block worst represented by the seeds so far,
           weighted by how often it occurs. Rare but distinct details (outlines, highlights) get
           codewords of their own instead of being averaged away. */
        int32_t* order = (int32_t*)malloc((size_t)nu * sizeof(int32_t));
        int no = 0;
        for (int u = 0; u < nu; u++) if (u != transparentU) order[no++] = u;
        int32_t* mind = (int32_t*)malloc((size_t)nu * sizeof(int32_t));
        {
            int best = order[0];
            for (int i = 1; i < no; i++) if (ucount[order[i]] > ucount[best]) best = order[i];
            for (int c = 0; c < k; c++) {
                for (int d = 0; d < DIM; d++) cen[c][d] = uvec[best * DIM + d];
                int64_t far = -1; int nextBest = best;
                for (int i = 0; i < no; i++) {
                    int u = order[i];
                    int32_t dd = dist2(&uvec[u * DIM], &uvec[best * DIM]);
                    if (c == 0 || dd < mind[u]) mind[u] = dd;
                    uint32_t w = ucount[u]; int64_t wgt = w >= 16 ? 4 : (w >= 4 ? 2 : 1);
                    int64_t sc = (int64_t)mind[u] * wgt;
                    if (sc > far) { far = sc; nextBest = u; }
                }
                best = nextBest;
            }
        }
        free(mind);
        int16_t cen16[256][DIM];
        int64_t (*sum)[DIM] = malloc(sizeof(int64_t[DIM]) * 256);
        uint64_t* wsum = (uint64_t*)malloc(sizeof(uint64_t) * 256);
        for (int it = 0; it < 24; it++) {
            for (int c = 0; c < k; c++) for (int d = 0; d < DIM; d++) cen16[c][d] = (int16_t)cen[c][d];
            memset(sum, 0, sizeof(int64_t[DIM]) * 256);
            memset(wsum, 0, sizeof(uint64_t) * 256);
            int changed = 0;
            for (int i = 0; i < no; i++) {
                int u = order[i];
                const int16_t* v = &uvec[u * DIM];
                int best = 0; int32_t bd = 0x7FFFFFFF;
                for (int c = 0; c < k; c++) { int32_t dd = dist2(v, cen16[c]); if (dd < bd) { bd = dd; best = c; } }
                if (it == 0 || assign[u] != best) changed++;
                assign[u] = best;
                for (int d = 0; d < DIM; d++) sum[best][d] += (int64_t)v[d] * ucount[u];
                wsum[best] += ucount[u];
            }
            int far = -1; int32_t farD = -1;
            for (int c = 0; c < k; c++) {
                if (wsum[c]) { for (int d = 0; d < DIM; d++) cen[c][d] = (int32_t)((sum[c][d] + (int64_t)wsum[c] / 2) / (int64_t)wsum[c]); }
                else {
                    /* empty cluster: reseed on the block that is worst represented */
                    if (far < 0) {
                        for (int i = 0; i < no; i++) { int u = order[i]; int32_t dd = dist2(&uvec[u * DIM], cen16[assign[u]]) * (int32_t)(ucount[u] > 8 ? 8 : ucount[u]); if (dd > farD) { farD = dd; far = u; } }
                    }
                    if (far >= 0) { for (int d = 0; d < DIM; d++) cen[c][d] = uvec[far * DIM + d]; far = -2; }
                }
            }
            if (it > 2 && changed == 0) break;
        }
        /* final codebook in 16 bits, then reassign against the exact codewords */
        ncode = 256;
        if (first) { code[0][0] = code[0][1] = code[0][2] = code[0][3] = 0; }
        for (int c = 0; c < k; c++)
            for (int t = 0; t < 4; t++) code[first + c][t] = pack16(&cen[c][t * 4], soft);
        for (int c = 0; c < ncode; c++) for (int t = 0; t < 4; t++) unpack16(code[c][t], soft, &cvec[c][t * 4]);
        for (int u = 0; u < nu; u++) {
            if (u == transparentU) { assign[u] = 0; continue; }
            int best = first; int32_t bd = 0x7FFFFFFF;
            for (int c = first; c < ncode; c++) { int32_t dd = dist2(&uvec[u * DIM], cvec[c]); if (dd < bd) { bd = dd; best = c; } }
            assign[u] = best;
        }
        free(cen); free(sum); free(wsum); free(order);
    }

    /* error report (mean squared error per channel over opaque texels) */
    if (outErr) {
        double e = 0; uint64_t n = 0;
        for (int c = 0; c < ncode; c++) for (int t = 0; t < 4; t++) unpack16(code[c][t], soft, &cvec[c][t * 4]);
        for (int u = 0; u < nu; u++) {
            if (u == transparentU) continue;
            e += (double)dist2(&uvec[u * DIM], cvec[assign[u]]) * ucount[u];
            n += (uint64_t)ucount[u] * DIM;
        }
        *outErr = n ? e / (double)n : 0.0;
    }

    /* codebook: 256 entries x 4 texels, little-endian */
    memset(out, 0, 2048);
    for (int c = 0; c < ncode; c++)
        for (int t = 0; t < 4; t++) { out[c * 8 + t * 2] = (uint8_t)(code[c][t] & 0xFF); out[c * 8 + t * 2 + 1] = (uint8_t)(code[c][t] >> 8); }
    /* index grid, twiddled (y bit lowest) */
    uint8_t* idx = out + 2048;
    for (int by = 0; by < bw; by++)
        for (int bx = 0; bx < bw; bx++)
            idx[twid((uint32_t)by) | (twid((uint32_t)bx) << 1)] = (uint8_t)assign[blockId[by * bw + bx]];

    free(keys); free(blockId); free(tab); free(ukey); free(ucount); free(uvec); free(assign);
    return 0;
}
