/* Disc image writer: ISO9660 filesystem + GD-ROM (.gdi) layout.
 *
 * A Dreamcast GD-ROM has a low-density area (tracks 1-2) and a
 * high-density area starting at LBA 45000 (track 3). The boot sector
 * (IP.BIN, 16 sectors) sits at the very start of track 3, followed by the
 * ISO9660 volume whose extents are absolute LBAs (45000 + n).
 *
 * Data tracks are written as raw 2352-byte Mode 1 sectors (sync, header,
 * EDC, ECC) so every emulator and GDEMU accepts them.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SECTOR 2048
#define RAW 2352

/* ---------------- Mode 1 EDC/ECC ---------------- */
static uint8_t ecc_f_lut[256], ecc_b_lut[256];
static uint32_t edc_lut[256];
static void ecc_init(void) {
    static int done = 0;
    if (done) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
        ecc_f_lut[i] = (uint8_t)j;
        ecc_b_lut[i ^ j] = (uint8_t)i;
        uint32_t edc = i;
        for (j = 0; j < 8; j++) edc = (edc >> 1) ^ (edc & 1 ? 0xD8018001u : 0);
        edc_lut[i] = edc;
    }
    done = 1;
}
static uint32_t edc_compute(const uint8_t* src, size_t n) {
    uint32_t edc = 0;
    while (n--) edc = (edc >> 8) ^ edc_lut[(edc ^ (*src++)) & 0xFF];
    return edc;
}
static void ecc_block(const uint8_t* src, uint32_t majorCount, uint32_t minorCount, uint32_t majorMult, uint32_t minorInc, uint8_t* dest) {
    uint32_t size = majorCount * minorCount;
    for (uint32_t major = 0; major < majorCount; major++) {
        uint32_t index = (major >> 1) * majorMult + (major & 1);
        uint8_t a = 0, b = 0;
        for (uint32_t minor = 0; minor < minorCount; minor++) {
            uint8_t t = src[index];
            index += minorInc;
            if (index >= size) index -= size;
            a ^= t; b ^= t;
            a = ecc_f_lut[a];
        }
        a = ecc_b_lut[ecc_f_lut[a] ^ b];
        dest[major] = a;
        dest[major + majorCount] = a ^ b;
    }
}
static void lba_to_msf(uint32_t lba, uint8_t* m, uint8_t* s, uint8_t* f) {
    lba += 150;
    *m = (uint8_t)(lba / (75 * 60)); *s = (uint8_t)((lba / 75) % 60); *f = (uint8_t)(lba % 75);
}
static uint8_t bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void make_raw_sector(uint8_t* out, const uint8_t* data, uint32_t lba) {
    ecc_init();
    static const uint8_t sync[12] = {0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0};
    memcpy(out, sync, 12);
    uint8_t m, s, f; lba_to_msf(lba, &m, &s, &f);
    out[12] = bcd(m); out[13] = bcd(s); out[14] = bcd(f); out[15] = 1;
    memcpy(out + 16, data, SECTOR);
    uint32_t edc = edc_compute(out, 16 + SECTOR);
    out[2064] = (uint8_t)edc; out[2065] = (uint8_t)(edc >> 8); out[2066] = (uint8_t)(edc >> 16); out[2067] = (uint8_t)(edc >> 24);
    memset(out + 2068, 0, 8);
    ecc_block(out + 12, 86, 24, 2, 86, out + 2076);   /* P */
    ecc_block(out + 12, 52, 43, 86, 88, out + 2248);  /* Q */
}

/* ---------------- track writer ---------------- */
typedef struct {
    FILE* f;
    uint32_t lba;      /* absolute LBA of next sector */
    int raw;
    uint64_t bytes;
} Track;

static int track_put(Track* t, const uint8_t* data) {
    if (t->raw) {
        uint8_t rs[RAW];
        make_raw_sector(rs, data, t->lba);
        if (fwrite(rs, 1, RAW, t->f) != RAW) return -1;
        t->bytes += RAW;
    } else {
        if (fwrite(data, 1, SECTOR, t->f) != SECTOR) return -1;
        t->bytes += SECTOR;
    }
    t->lba++;
    return 0;
}

/* ---------------- ISO9660 ---------------- */
static void both16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void both32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16); p[6] = (uint8_t)(v >> 8); p[7] = (uint8_t)v;
}
static void le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void be32(uint8_t* p, uint32_t v) { p[3] = (uint8_t)v; p[2] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v >> 16); p[0] = (uint8_t)(v >> 24); }
static void padstr(uint8_t* p, const char* s, int n) { int l = (int)strlen(s); for (int i = 0; i < n; i++) p[i] = (uint8_t)(i < l ? s[i] : ' '); }

static void dir_date(uint8_t* p) {
    time_t now = time(NULL);
    struct tm* t = gmtime(&now);
    p[0] = (uint8_t)(t->tm_year); p[1] = (uint8_t)(t->tm_mon + 1); p[2] = (uint8_t)t->tm_mday;
    p[3] = (uint8_t)t->tm_hour; p[4] = (uint8_t)t->tm_min; p[5] = (uint8_t)t->tm_sec; p[6] = 0;
}
static void vol_date(uint8_t* p) {
    time_t now = time(NULL);
    struct tm* t = gmtime(&now);
    char b[20];
    snprintf(b, sizeof b, "%04d%02d%02d%02d%02d%02d00", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    memcpy(p, b, 16); p[16] = 0;
}

static int dir_record(uint8_t* p, const char* name, int nameLen, uint32_t lba, uint32_t size, int isDir) {
    int len = 33 + nameLen;
    if (len & 1) len++;
    memset(p, 0, (size_t)len);
    p[0] = (uint8_t)len;
    both32(p + 2, lba);
    both32(p + 10, size);
    dir_date(p + 18);
    p[25] = isDir ? 2 : 0;
    both16(p + 28, 1);
    p[32] = (uint8_t)nameLen;
    memcpy(p + 33, name, (size_t)nameLen);
    return len;
}

typedef struct { const DiscFile* df; uint32_t size; uint32_t lba; char isoName[40]; } IsoEntry;

static int cmp_iso(const void* a, const void* b) { return strcmp(((const IsoEntry*)a)->isoName, ((const IsoEntry*)b)->isoName); }

static uint32_t file_size(const DiscFile* df) {
    if (df->mem) return df->memSize;
    FILE* f = fopen(df->srcPath, "rb");
    if (!f) return 0xFFFFFFFFu;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return (uint32_t)n;
}

/* Writes a complete ISO9660 volume into track t whose first sector has absolute LBA t->lba.
 * bootArea (may be NULL) fills the 16-sector system area. */
static int write_iso(BuildContext* ctx, Track* t, const uint8_t* bootArea, size_t bootSize,
                     const DiscFile* files, int fileCount, const char* volName, int minSectors, double progBase, double progSpan) {
    uint32_t base = t->lba;
    IsoEntry* ents = (IsoEntry*)calloc((size_t)fileCount + 1, sizeof(IsoEntry));
    for (int i = 0; i < fileCount; i++) {
        ents[i].df = &files[i];
        snprintf(ents[i].isoName, sizeof ents[i].isoName, "%s;1", files[i].isoName);
        ents[i].size = file_size(&files[i]);
        if (ents[i].size == 0xFFFFFFFFu) { build_log(ctx, "error: cannot read %s", files[i].srcPath); free(ents); return -1; }
    }
    qsort(ents, (size_t)fileCount, sizeof(IsoEntry), cmp_iso);

    /* root directory size */
    uint32_t dirBytes = 34 + 34;
    for (int i = 0; i < fileCount; i++) {
        int l = 33 + (int)strlen(ents[i].isoName); if (l & 1) l++;
        if ((dirBytes % SECTOR) + (uint32_t)l > SECTOR) dirBytes = (dirBytes / SECTOR + 1) * SECTOR;
        dirBytes += (uint32_t)l;
    }
    uint32_t dirSectors = (dirBytes + SECTOR - 1) / SECTOR;
    uint32_t rel = 16 + 2 + 2;             /* system area, PVD, terminator, 2 path tables */
    uint32_t rootLba = base + rel;
    rel += dirSectors;
    /* 1ST_READ.BIN last (outermost) is tradition; order otherwise by name */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < fileCount; i++) {
            int isBoot = !strcmp(ents[i].isoName, "1ST_READ.BIN;1");
            if (isBoot != pass) continue;
            ents[i].lba = base + rel;
            rel += (ents[i].size + SECTOR - 1) / SECTOR;
            if (ents[i].size == 0) rel += 0;
        }
    uint32_t total = rel;
    if (total < (uint32_t)minSectors) total = (uint32_t)minSectors;

    uint8_t sec[SECTOR];
    /* system area */
    for (int i = 0; i < 16; i++) {
        memset(sec, 0, SECTOR);
        if (bootArea && (size_t)i * SECTOR < bootSize) {
            size_t n = bootSize - (size_t)i * SECTOR; if (n > SECTOR) n = SECTOR;
            memcpy(sec, bootArea + (size_t)i * SECTOR, n);
        }
        if (track_put(t, sec)) goto ioerr;
    }
    /* PVD */
    memset(sec, 0, SECTOR);
    sec[0] = 1; memcpy(sec + 1, "CD001", 5); sec[6] = 1;
    padstr(sec + 8, "", 32);
    padstr(sec + 40, volName, 32);
    both32(sec + 80, base + total);
    both16(sec + 120, 1); both16(sec + 124, 1); both16(sec + 128, SECTOR);
    both32(sec + 132, 10);                 /* path table size: one root entry */
    le32(sec + 140, base + 18); le32(sec + 144, 0);
    be32(sec + 148, base + 19); be32(sec + 152, 0);
    dir_record(sec + 156, "\0", 1, rootLba, dirSectors * SECTOR, 1);
    padstr(sec + 190, "", 128); padstr(sec + 318, "UTDC", 128); padstr(sec + 446, "UTDC BUILDER", 128); padstr(sec + 574, "UTDC", 128);
    padstr(sec + 702, "", 37); padstr(sec + 739, "", 37); padstr(sec + 776, "", 37);
    vol_date(sec + 813); vol_date(sec + 830);
    memset(sec + 847, '0', 16); sec[863] = 0; memset(sec + 864, '0', 16); sec[880] = 0;
    sec[881] = 1;
    if (track_put(t, sec)) goto ioerr;
    /* terminator */
    memset(sec, 0, SECTOR); sec[0] = 255; memcpy(sec + 1, "CD001", 5); sec[6] = 1;
    if (track_put(t, sec)) goto ioerr;
    /* path tables */
    memset(sec, 0, SECTOR); sec[0] = 1; le32(sec + 2, rootLba); sec[6] = 1; sec[7] = 0;
    if (track_put(t, sec)) goto ioerr;
    memset(sec, 0, SECTOR); sec[0] = 1; be32(sec + 2, rootLba); sec[6] = 0; sec[7] = 1;
    if (track_put(t, sec)) goto ioerr;
    /* root dir */
    {
        uint8_t* dir = (uint8_t*)calloc(dirSectors, SECTOR);
        uint32_t p = 0;
        p += (uint32_t)dir_record(dir + p, "\0", 1, rootLba, dirSectors * SECTOR, 1);
        p += (uint32_t)dir_record(dir + p, "\1", 1, rootLba, dirSectors * SECTOR, 1);
        for (int i = 0; i < fileCount; i++) {
            int l = 33 + (int)strlen(ents[i].isoName); if (l & 1) l++;
            if ((p % SECTOR) + (uint32_t)l > SECTOR) p = (p / SECTOR + 1) * SECTOR;
            p += (uint32_t)dir_record(dir + p, ents[i].isoName, (int)strlen(ents[i].isoName), ents[i].lba, ents[i].size, 0);
        }
        for (uint32_t s = 0; s < dirSectors; s++) if (track_put(t, dir + (size_t)s * SECTOR)) { free(dir); goto ioerr; }
        free(dir);
    }
    /* file data, in LBA order */
    uint64_t written = 0, toWrite = 0;
    for (int i = 0; i < fileCount; i++) toWrite += ents[i].size;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < fileCount; i++) {
            int isBoot = !strcmp(ents[i].isoName, "1ST_READ.BIN;1");
            if (isBoot != pass) continue;
            if (t->lba != ents[i].lba) { build_log(ctx, "internal error: LBA mismatch"); free(ents); return -1; }
            const DiscFile* df = ents[i].df;
            FILE* src = NULL;
            if (!df->mem) { src = fopen(df->srcPath, "rb"); if (!src) goto ioerr; }
            uint32_t left = ents[i].size, off = 0;
            while (left > 0) {
                memset(sec, 0, SECTOR);
                uint32_t n = left > SECTOR ? SECTOR : left;
                if (df->mem) memcpy(sec, df->mem + off, n);
                else if (fread(sec, 1, n, src) != n) { fclose(src); goto ioerr; }
                if (track_put(t, sec)) { if (src) fclose(src); goto ioerr; }
                left -= n; off += n; written += n;
                if ((off & 0xFFFFF) == 0) {
                    if (build_cancelled(ctx)) { if (src) fclose(src); free(ents); return -1; }
                    build_progress(ctx, progBase + progSpan * (double)written / (toWrite ? toWrite : 1), "Writing disc image (%s)", df->isoName);
                }
            }
            if (src) fclose(src);
        }
    memset(sec, 0, SECTOR);
    while (t->lba < base + total) if (track_put(t, sec)) goto ioerr;
    free(ents);
    return 0;
ioerr:
    build_log(ctx, "error: I/O error while writing disc image");
    free(ents);
    return -1;
}

static void path_join_dir(char* out, size_t outLen, const char* filePath, const char* name) {
    const char* slash = NULL;
    for (const char* c = filePath; *c; c++) if (*c == '/' || *c == '\\') slash = c;
    if (slash) snprintf(out, outLen, "%.*s/%s", (int)(slash - filePath), filePath, name);
    else snprintf(out, outLen, "%s", name);
}

static int write_gdi(BuildContext* ctx, const char* outPath, const uint8_t* ip, size_t ipSize,
                     const DiscFile* files, int fileCount, const char* volName) {
    char p1[1024], p2[1024], p3[1024];
    path_join_dir(p1, sizeof p1, outPath, "track01.bin");
    path_join_dir(p2, sizeof p2, outPath, "track02.raw");
    path_join_dir(p3, sizeof p3, outPath, "track03.bin");

    /* Track 1: small low-density data track with a readme */
    static const char readme[] =
        "This disc image was generated by the UTDC builder from a legally owned copy of the game.\r\n"
        "Do not distribute it.\r\n";
    DiscFile rd = { "README.TXT", NULL, (const uint8_t*)readme, (uint32_t)(sizeof(readme) - 1) };
    Track t = {0};
    t.raw = 1; t.lba = 0;
    t.f = fopen(p1, "wb");
    if (!t.f) { build_log(ctx, "error: cannot create %s", p1); return -1; }
    if (write_iso(ctx, &t, NULL, 0, &rd, 1, volName, 300, 0.85, 0.0)) { fclose(t.f); return -1; }
    fclose(t.f);
    uint32_t t1len = t.lba;

    /* Track 2: audio silence (4 s) */
    FILE* f = fopen(p2, "wb");
    if (!f) { build_log(ctx, "error: cannot create %s", p2); return -1; }
    uint8_t zero[RAW]; memset(zero, 0, RAW);
    for (int i = 0; i < 300; i++) fwrite(zero, 1, RAW, f);
    fclose(f);
    uint32_t t2lba = t1len + 150;

    /* Track 3: high-density area with IP.BIN + game files */
    t.raw = 1; t.lba = 45000; t.bytes = 0;
    t.f = fopen(p3, "wb");
    if (!t.f) { build_log(ctx, "error: cannot create %s", p3); return -1; }
    if (write_iso(ctx, &t, ip, ipSize, files, fileCount, volName, 0, 0.85, 0.14)) { fclose(t.f); return -1; }
    fclose(t.f);
    ctx->stats.discBytes = t.bytes;

    f = fopen(outPath, "wb");
    if (!f) { build_log(ctx, "error: cannot create %s", outPath); return -1; }
    fprintf(f, "3\n1 0 4 2352 track01.bin 0\n2 %u 0 2352 track02.raw 0\n3 45000 4 2352 track03.bin 0\n", t2lba);
    fclose(f);
    build_log(ctx, "Wrote %s (+ track01.bin, track02.raw, track03.bin: %.1f MB)", outPath, t.bytes / 1048576.0);
    return 0;
}

int disc_write(BuildContext* ctx, DiscFormat fmt, const char* outPath,
               const uint8_t* ipBin, size_t ipSize,
               const DiscFile* files, int fileCount, const char* volumeName) {
    if (fmt == DISC_FORMAT_GDI) return write_gdi(ctx, outPath, ipBin, ipSize, files, fileCount, volumeName);
    build_log(ctx, "error: CDI output is not available in this build, use GDI");
    return -1;
}
