/* Minimal GameMaker data.win reader used by the ROM builder.
 * Only reads what the builder needs: chunk table, TXTR, TPAG, SPRT (frame ownership), SOND, AUDO.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef UTDC_DATAWIN_H
#define UTDC_DATAWIN_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    char     name[5];
    uint32_t offset;  /* offset of chunk payload (after 8-byte header) */
    uint32_t size;
} DwChunk;

typedef struct {
    uint16_t srcX, srcY, srcW, srcH;
    uint16_t tgtX, tgtY, tgtW, tgtH;
    uint16_t bndW, bndH;
    int16_t  page;
} DwTpag;

typedef struct {
    uint32_t offset;  /* PNG blob absolute offset */
    uint32_t size;
} DwTexture;

typedef struct {
    const char* name;
    uint32_t flags;
    const char* type;
    const char* file;
    float volume, pitch;
    int32_t group;
    int32_t audioFile;
} DwSound;

typedef struct {
    uint32_t offset;  /* absolute offset of audio bytes */
    uint32_t size;
} DwAudio;

typedef struct {
    uint8_t* data;
    size_t   size;
    uint32_t wadVersion;
    const char* displayName;

    DwChunk  chunks[64];
    int      chunkCount;

    DwTpag*    tpag;    uint32_t tpagCount;
    DwTexture* tex;     uint32_t texCount;
    DwSound*   sound;   uint32_t soundCount;
    DwAudio*   audio;   uint32_t audioCount;
    /* per TPAG: owning sprite (-1 for fonts/backgrounds) and frame index, from SPRT */
    int32_t*   tpagSprite;
    int32_t*   tpagFrame;
} DataWinFile;

/* Returns 0 on success; on failure, writes a message into err. */
int  dw_load(DataWinFile* dw, const char* path, char* err, size_t errLen);
void dw_free(DataWinFile* dw);
const DwChunk* dw_chunk(const DataWinFile* dw, const char* name);

/* Writes a copy of data.win with the TXTR and AUDO payloads emptied
 * (they are replaced by TEXTURES.BIN / AUDIO.BIN on the disc). */
int  dw_write_stripped(const DataWinFile* dw, const char* outPath);

#endif
