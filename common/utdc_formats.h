/*
 * utdc_formats.h - On-disc asset formats shared by the ROM builder (PC side)
 * and the Dreamcast runner (console side).
 *
 * All integers are little-endian (both x86 and SH-4 are little-endian, so the
 * structs below can be read directly with fread on either side).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef UTDC_FORMATS_H
#define UTDC_FORMATS_H

#include <stdint.h>

#define UTDC_FORMAT_VERSION 4

/* ------------------------------------------------------------------------
 * TEXTURES.BIN
 *
 *   UtdcTexHeader
 *   UtdcTexPage   pages[pageCount]
 *   UtdcTexItem   items[itemCount]      (one per data.win TPAG entry, same index)
 *   ...page payloads...
 *
 * Every page is a power-of-two texture (8..1024 per side) already twiddled
 * for the PowerVR2, in ARGB1555 (binary alpha) or ARGB4444 (soft alpha).
 * Pages with UTDC_TEXFMT_VQ set are square VQ-compressed textures: a 256-entry
 * codebook (2x2 texels each, 2 KB) followed by one twiddled index byte per
 * 2x2 block. They hold only photographic, many-coloured sprite frames, which
 * VQ reproduces closely at 1/8 of the VRAM.
 *
 * A page payload is a sequence of independent LZ4 blocks (fast to decode on
 * the SH-4), each preceded by a UtdcTexBlock header. Every block decodes to
 * at most UTDC_TEX_BLOCK_RAW bytes, a multiple of 4 except possibly the last.
 * ---------------------------------------------------------------------- */
#define UTDC_TEX_MAGIC 0x54584455u /* "UDXT" */

enum {
    UTDC_TEXFMT_ARGB1555 = 0,
    UTDC_TEXFMT_ARGB4444 = 1,
    UTDC_TEXFMT_RGB565   = 2
};
#define UTDC_TEXFMT_VQ 0x80   /* flag on top of the pixel format */

/* VRAM bytes of a page's decoded payload */
#define UTDC_TEX_PAGE_BYTES(w, h, fmt) (((fmt) & UTDC_TEXFMT_VQ) ? 2048u + (uint32_t)(w) * (uint32_t)(h) / 4u : (uint32_t)(w) * (uint32_t)(h) * 2u)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t pageCount;
    uint32_t itemCount;
    uint32_t largestPageBytes;   /* biggest uncompressed page, for staging buffer sizing */
    uint32_t largestCompBytes;   /* biggest compressed page payload */
    uint32_t reserved[2];
} UtdcTexHeader;

typedef struct {
    uint32_t offset;     /* absolute file offset of the page payload */
    uint32_t compSize;   /* bytes of payload (all blocks with headers) */
    uint16_t width;      /* texels, power of two */
    uint16_t height;     /* texels, power of two */
    uint8_t  format;     /* UTDC_TEXFMT_* */
    uint8_t  reserved[3];
} UtdcTexPage;

#define UTDC_TEX_BLOCK_RAW (32 * 1024)

typedef struct {
    uint32_t compSize;   /* bytes of LZ4 block data that follow */
    uint32_t rawSize;    /* bytes it decodes to */
} UtdcTexBlock;

#define UTDC_TEX_NO_PAGE 0xFFFFu

typedef struct {
    uint16_t page;       /* UTDC_TEX_NO_PAGE if the item is empty/missing */
    uint16_t x, y;       /* texel position inside the page */
    uint16_t w, h;       /* texel size inside the page (== TPAG source size unless downscaled) */
    uint16_t reserved;
} UtdcTexItem;

/* ------------------------------------------------------------------------
 * AUDIO.BIN
 *
 *   UtdcAudHeader
 *   int32_t       soundToEntry[soundCount]   (SOND index -> entry, -1 if none)
 *   UtdcAudEntry  entries[entryCount]
 *   char          names[nameBytes]           (NUL-separated stream keys, see below)
 *   ...ADPCM payloads, each 32-byte aligned...
 *
 * Audio is 4-bit Yamaha/AICA ADPCM. Sound effects are mono and get uploaded
 * to sound RAM on demand; streams (music) are mono or nibble-interleaved
 * stereo (KOS snd_stream layout: byte = L<<4 | R) and are read from disc.
 *
 * Stream keys let audio_create_stream("music/story.ogg") find the right
 * entry: the key is the lower-cased base file name with any directory,
 * "mus_" prefix and extension removed ("story").
 * ---------------------------------------------------------------------- */
#define UTDC_AUD_MAGIC 0x44554155u /* "UAUD" */

enum {
    UTDC_AUDKIND_SFX    = 0,
    UTDC_AUDKIND_STREAM = 1
};

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t soundCount;
    uint32_t entryCount;
    uint32_t nameBytes;
    uint32_t reserved[3];
} UtdcAudHeader;

typedef struct {
    uint32_t offset;     /* absolute file offset */
    uint32_t size;       /* bytes of ADPCM data */
    uint32_t rate;       /* sample rate in Hz */
    uint32_t samples;    /* samples per channel */
    uint8_t  channels;   /* 1 or 2 */
    uint8_t  kind;       /* UTDC_AUDKIND_* */
    uint16_t reserved;
    uint32_t nameOffset; /* offset into names[] or 0xFFFFFFFF */
} UtdcAudEntry;

/* ------------------------------------------------------------------------
 * UTDC.CFG - tiny key=value text file with runner settings written by the
 * builder (game title, which rooms to keep resident, etc).
 * ---------------------------------------------------------------------- */

#endif /* UTDC_FORMATS_H */
