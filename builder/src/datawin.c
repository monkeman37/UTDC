/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "datawin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static float rdf(const uint8_t* p) { uint32_t v = rd32(p); float f; memcpy(&f, &v, 4); return f; }

static const char* gmstr(const DataWinFile* dw, uint32_t ptr) {
    if (ptr == 0 || ptr >= dw->size) return "";
    return (const char*)dw->data + ptr;
}

const DwChunk* dw_chunk(const DataWinFile* dw, const char* name) {
    for (int i = 0; i < dw->chunkCount; i++)
        if (memcmp(dw->chunks[i].name, name, 4) == 0) return &dw->chunks[i];
    return NULL;
}

/* Reads a GameMaker pointer list at chunk start: count + count*u32 */
static uint32_t ptrlist(const DataWinFile* dw, const DwChunk* c, const uint8_t** out) {
    if (!c || c->size < 4) { *out = NULL; return 0; }
    *out = dw->data + c->offset + 4;
    return rd32(dw->data + c->offset);
}

int dw_load(DataWinFile* dw, const char* path, char* err, size_t errLen) {
    memset(dw, 0, sizeof(*dw));
    FILE* f = fopen(path, "rb");
    if (!f) { snprintf(err, errLen, "Cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 16) { fclose(f); snprintf(err, errLen, "data.win is too small"); return -1; }
    dw->data = (uint8_t*)malloc((size_t)sz);
    if (!dw->data) { fclose(f); snprintf(err, errLen, "Out of memory"); return -1; }
    if (fread(dw->data, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); snprintf(err, errLen, "Read error"); return -1; }
    fclose(f);
    dw->size = (size_t)sz;

    if (memcmp(dw->data, "FORM", 4) != 0) { snprintf(err, errLen, "Not a GameMaker data.win (missing FORM header)"); return -1; }
    size_t off = 8;
    while (off + 8 <= dw->size && dw->chunkCount < 64) {
        DwChunk* c = &dw->chunks[dw->chunkCount++];
        memcpy(c->name, dw->data + off, 4); c->name[4] = 0;
        c->size = rd32(dw->data + off + 4);
        c->offset = (uint32_t)(off + 8);
        off += 8 + (size_t)c->size;
    }

    const DwChunk* gen8 = dw_chunk(dw, "GEN8");
    if (!gen8) { snprintf(err, errLen, "data.win has no GEN8 chunk"); return -1; }
    dw->wadVersion = dw->data[gen8->offset + 1];
    {
        /* GEN8 layout: u8 debug, u8 wadver, u16 pad, ptr filename, ptr config, u32 lastObj, u32 lastTile,
           u32 gameId, guid[16], ptr name, ... */
        const uint8_t* g = dw->data + gen8->offset;
        dw->displayName = gmstr(dw, rd32(g + 4 + 4 + 4 + 4 + 4 + 4 + 16));
    }

    const uint8_t* p;
    /* TPAG */
    uint32_t n = ptrlist(dw, dw_chunk(dw, "TPAG"), &p);
    dw->tpagCount = n;
    dw->tpag = (DwTpag*)calloc(n ? n : 1, sizeof(DwTpag));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ptr = rd32(p + i * 4);
        DwTpag* t = &dw->tpag[i];
        if (!ptr) { t->page = -1; continue; }
        const uint8_t* q = dw->data + ptr;
        t->srcX = rd16(q + 0);  t->srcY = rd16(q + 2);  t->srcW = rd16(q + 4);  t->srcH = rd16(q + 6);
        t->tgtX = rd16(q + 8);  t->tgtY = rd16(q + 10); t->tgtW = rd16(q + 12); t->tgtH = rd16(q + 14);
        t->bndW = rd16(q + 16); t->bndH = rd16(q + 18); t->page = (int16_t)rd16(q + 20);
    }

    /* SPRT: which sprite/frame each TPAG belongs to (lets the packer keep animation frames
       together). GMS 1.x layout: 14 u32 header fields, then u32 frame count and frame TPAG
       pointers; a count of 0xFFFFFFFF marks the GMS2 "special" layout, which is skipped. */
    dw->tpagSprite = (int32_t*)malloc((dw->tpagCount ? dw->tpagCount : 1) * sizeof(int32_t));
    dw->tpagFrame = (int32_t*)malloc((dw->tpagCount ? dw->tpagCount : 1) * sizeof(int32_t));
    for (uint32_t i = 0; i < dw->tpagCount; i++) { dw->tpagSprite[i] = -1; dw->tpagFrame[i] = 0; }
    {
        const uint8_t* tp; uint32_t tn = ptrlist(dw, dw_chunk(dw, "TPAG"), &tp);
        const uint8_t* sp; uint32_t sn = ptrlist(dw, dw_chunk(dw, "SPRT"), &sp);
        for (uint32_t s = 0; s < sn; s++) {
            uint32_t ptr = rd32(sp + s * 4);
            if (!ptr || (size_t)ptr + 64 > dw->size) continue;
            uint32_t cnt = rd32(dw->data + ptr + 56);
            if (cnt == 0xFFFFFFFFu || cnt > 4096 || (size_t)ptr + 60 + (size_t)cnt * 4 > dw->size) continue;
            for (uint32_t f = 0; f < cnt; f++) {
                uint32_t tptr = rd32(dw->data + ptr + 60 + f * 4);
                /* TPAG entries are stored in order, so the pointer list is sorted: binary search */
                uint32_t lo = 0, hi = tn;
                while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (rd32(tp + mid * 4) < tptr) lo = mid + 1; else hi = mid; }
                if (lo < tn && rd32(tp + lo * 4) == tptr && dw->tpagSprite[lo] < 0) { dw->tpagSprite[lo] = (int32_t)s; dw->tpagFrame[lo] = (int32_t)f; }
            }
        }
    }

    /* TXTR: GMS 1.x entries are {u32 scaled, u32 blobOffset}; GMS2 adds generatedMips etc. */
    const DwChunk* txtr = dw_chunk(dw, "TXTR");
    n = ptrlist(dw, txtr, &p);
    dw->texCount = n;
    dw->tex = (DwTexture*)calloc(n ? n : 1, sizeof(DwTexture));
    uint32_t entrySize = 8;
    if (n >= 2) {
        uint32_t diff = rd32(p + 4) - rd32(p);
        if (diff == 12 || diff == 16 || diff == 28) entrySize = diff;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ptr = rd32(p + i * 4);
        if (!ptr) continue;
        uint32_t blobPtrOff = (entrySize == 8) ? 4 : 8;
        dw->tex[i].offset = rd32(dw->data + ptr + blobPtrOff);
    }
    /* Blob sizes: distance to next blob (or chunk end). PNG decoders stop at IEND anyway. */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t start = dw->tex[i].offset, end = txtr->offset + txtr->size;
        for (uint32_t j = 0; j < n; j++)
            if (dw->tex[j].offset > start && dw->tex[j].offset < end) end = dw->tex[j].offset;
        dw->tex[i].size = start ? end - start : 0;
    }

    /* SOND */
    n = ptrlist(dw, dw_chunk(dw, "SOND"), &p);
    dw->soundCount = n;
    dw->sound = (DwSound*)calloc(n ? n : 1, sizeof(DwSound));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ptr = rd32(p + i * 4);
        DwSound* s = &dw->sound[i];
        s->audioFile = -1;
        if (!ptr) { s->name = ""; s->file = ""; s->type = ""; continue; }
        const uint8_t* q = dw->data + ptr;
        s->name = gmstr(dw, rd32(q + 0));
        s->flags = rd32(q + 4);
        s->type = gmstr(dw, rd32(q + 8));
        s->file = gmstr(dw, rd32(q + 12));
        s->volume = rdf(q + 20);
        s->pitch = rdf(q + 24);
        s->group = (int32_t)rd32(q + 28);
        s->audioFile = (int32_t)rd32(q + 32);
    }

    /* AUDO: each entry is u32 length + bytes */
    n = ptrlist(dw, dw_chunk(dw, "AUDO"), &p);
    dw->audioCount = n;
    dw->audio = (DwAudio*)calloc(n ? n : 1, sizeof(DwAudio));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ptr = rd32(p + i * 4);
        if (!ptr) continue;
        dw->audio[i].size = rd32(dw->data + ptr);
        dw->audio[i].offset = ptr + 4;
    }
    return 0;
}

void dw_free(DataWinFile* dw) {
    free(dw->data); free(dw->tpag); free(dw->tpagSprite); free(dw->tpagFrame); free(dw->tex); free(dw->sound); free(dw->audio);
    memset(dw, 0, sizeof(*dw));
}

static void wr32(FILE* f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

int dw_write_stripped(const DataWinFile* dw, const char* outPath) {
    FILE* f = fopen(outPath, "wb");
    if (!f) return -1;
    /* Find the first of TXTR/AUDO; everything after it must be TXTR/AUDO only,
       otherwise we copy the file unchanged (offsets inside other chunks are absolute). */
    int firstIdx = -1;
    for (int i = 0; i < dw->chunkCount; i++) {
        if (!memcmp(dw->chunks[i].name, "TXTR", 4) || !memcmp(dw->chunks[i].name, "AUDO", 4)) { firstIdx = i; break; }
    }
    int canStrip = firstIdx >= 0;
    for (int i = firstIdx; canStrip && i < dw->chunkCount; i++)
        if (memcmp(dw->chunks[i].name, "TXTR", 4) && memcmp(dw->chunks[i].name, "AUDO", 4)) canStrip = 0;
    if (!canStrip) {
        size_t w = fwrite(dw->data, 1, dw->size, f);
        fclose(f);
        return w == dw->size ? 0 : -1;
    }
    uint32_t keep = dw->chunks[firstIdx].offset - 8;
    uint32_t tail = 0;
    for (int i = firstIdx; i < dw->chunkCount; i++) tail += 12; /* header + count(0) */
    fwrite(dw->data, 1, 4, f);              /* FORM */
    wr32(f, keep - 8 + tail);               /* new FORM size */
    fwrite(dw->data + 8, 1, keep - 8, f);
    for (int i = firstIdx; i < dw->chunkCount; i++) {
        fwrite(dw->chunks[i].name, 1, 4, f);
        wr32(f, 4);
        wr32(f, 0);
    }
    int ok = !ferror(f);
    fclose(f);
    return ok ? 0 : -1;
}
