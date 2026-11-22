/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <kos.h>
#include <dc/vmu_pkg.h>
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <kos/thread.h>
#include <kos/sem.h>
#include <kos/mutex.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#include "dc_file_system.h"
#include "utils.h"
#include "log.h"
#include "miniz.h"
#include "stb_ds.h"

#define SAVE_NAME "UTDC_SAV"
#define SAVE_MAGIC 0x53445455u /* "UTDS" */
#define FLUSH_DELAY_MS 1500

typedef struct {
    char* name;      /* normalised: lower case, forward slashes, no leading ./ */
    uint8_t* data;
    int32_t size;
} MemFile;

typedef struct {
    char* name;      /* normalised name of the backing MemFile (looked up on each access) */
    bool hasFile;
    int32_t pos;
    int32_t mode;
    bool readOnlyCopy;
    uint8_t* roData; int32_t roSize;
} MemHandle;

typedef struct {
    FileSystem base;
    MemFile* files; int32_t count, cap;
    char title[64];
    char discRoot[32];
    bool dirty;
    uint64_t dirtySince;
    bool unsaved;
    char status[64];
    /* background VMU writer */
    kthread_t* writer;
    semaphore_t workSem;
    mutex_t jobLock;
    uint8_t* jobData;
    int jobSize;
    volatile bool busy;
    volatile bool writeFailed;
} DcFs;

/* ------------------------------------------------------------------ */

static char* normName(const char* p) {
    while (*p == '.' && (p[1] == '/' || p[1] == '\\')) p += 2;
    while (*p == '/' || *p == '\\') p++;
    size_t n = strlen(p);
    char* o = (char*) safeMalloc(n + 1);
    for (size_t i = 0; i <= n; i++) o[i] = (char) (p[i] == '\\' ? '/' : tolower((unsigned char) p[i]));
    return o;
}

static MemFile* findFile(DcFs* fs, const char* rel) {
    char* n = normName(rel);
    MemFile* f = nullptr;
    for (int32_t i = 0; i < fs->count; i++) if (!strcmp(fs->files[i].name, n)) { f = &fs->files[i]; break; }
    free(n);
    return f;
}

static MemFile* createFile(DcFs* fs, const char* rel) {
    MemFile* f = findFile(fs, rel);
    if (f) return f;
    if (fs->count == fs->cap) {
        fs->cap = fs->cap ? fs->cap * 2 : 16;
        fs->files = (MemFile*) safeRealloc(fs->files, (size_t) fs->cap * sizeof(MemFile));
    }
    f = &fs->files[fs->count++];
    f->name = normName(rel);
    f->data = nullptr;
    f->size = 0;
    return f;
}

static void markDirty(DcFs* fs) {
    if (!fs->dirty) fs->dirtySince = timer_ms_gettime64();
    fs->dirty = true;
}

static bool readDisc(DcFs* fs, const char* rel, uint8_t** out, int32_t* outSize) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", fs->discRoot, rel);
    for (char* c = path + strlen(fs->discRoot) + 1; *c; c++) { if (*c == '\\') *c = '/'; else *c = (char) toupper((unsigned char) *c); }
    file_t f = fs_open(path, O_RDONLY);
    if (f == FILEHND_INVALID) return false;
    ssize_t n = (ssize_t) fs_total(f);
    uint8_t* b = (uint8_t*) safeMalloc((size_t) n + 1);
    ssize_t got = fs_read(f, b, (size_t) n);
    fs_close(f);
    if (got != n) { free(b); return false; }
    b[n] = 0;
    *out = b; *outSize = (int32_t) n;
    return true;
}

/* ------------------------------------------------------------------ */
/* VMU persistence                                                     */
/* ------------------------------------------------------------------ */

static maple_device_t* findVmu(char* outPath, size_t n) {
    for (int i = 0; ; i++) {
        maple_device_t* dev = maple_enum_type(i, MAPLE_FUNC_MEMCARD);
        if (!dev) return nullptr;
        snprintf(outPath, n, "/vmu/%c%d/%s", 'a' + dev->port, dev->unit, SAVE_NAME);
        return dev;
    }
}

static void buildIcon(vmu_pkg_t* pkg, uint8_t* iconData) {
    /* simple original heart icon, 32x32 4bpp */
    pkg->icon_pal[0] = 0xF000;  /* black */
    pkg->icon_pal[1] = 0xFF22;  /* red   */
    pkg->icon_pal[2] = 0xFFFF;  /* white */
    for (int i = 3; i < 16; i++) pkg->icon_pal[i] = 0xF000;
    memset(iconData, 0, 512);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            float fx = (x - 15.5f) / 11.0f, fy = (y - 13.0f) / 11.0f;
            float q = fx * fx + fy * fy - 1.0f;
            bool in = (q * q * q - fx * fx * fy * fy * fy) < 0.0f;
            int c = in ? 1 : 0;
            int idx = y * 32 + x;
            if (idx & 1) iconData[idx >> 1] |= (uint8_t) c;
            else iconData[idx >> 1] |= (uint8_t) (c << 4);
        }
    }
    pkg->icon_cnt = 1;
    pkg->icon_anim_speed = 0;
    pkg->icon_data = iconData;
}

/* Runs on the writer thread: the maple bus makes a VMU write take ~1 s, which must not stall the game. */
static void* vmuWriter(void* p) {
    DcFs* fs = (DcFs*) p;
    for (;;) {
        sem_wait(&fs->workSem);
        mutex_lock(&fs->jobLock);
        uint8_t* data = fs->jobData;
        int size = fs->jobSize;
        fs->jobData = nullptr;
        mutex_unlock(&fs->jobLock);
        if (!data) { fs->busy = false; continue; }
        uint64_t t0 = timer_ms_gettime64();
        char path[64];
        bool ok = false;
        if (findVmu(path, sizeof path)) {
            fs_unlink(path);
            file_t f = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
            if (f != FILEHND_INVALID) {
                ok = fs_write(f, data, (size_t) size) == size;
                fs_close(f);
            }
            if (ok) snprintf(fs->status, sizeof fs->status, "Saved to VMU (%d blocks)", (size + 511) / 512);
            else snprintf(fs->status, sizeof fs->status, "VMU write failed (full?) - need %d blocks", (size + 511) / 512);
        } else {
            snprintf(fs->status, sizeof fs->status, "No VMU found - progress not saved");
        }
        free(data);
        fs->unsaved = !ok;
        fs->writeFailed = !ok;
        logInfo("DC fs: %s (%u ms, background)\n", fs->status, (unsigned) (timer_ms_gettime64() - t0));
        fs->busy = false;
    }
    return nullptr;
}

/* Serialises all files into one compressed VMU package (main thread, a few ms). */
static uint8_t* buildPackage(DcFs* fs, int* outSize) {
    size_t raw = 8;
    for (int32_t i = 0; i < fs->count; i++) raw += 8 + strlen(fs->files[i].name) + (size_t) fs->files[i].size;
    uint8_t* buf = (uint8_t*) safeMalloc(raw);
    uint8_t* p = buf;
    uint32_t magic = SAVE_MAGIC, cnt = (uint32_t) fs->count;
    memcpy(p, &magic, 4); memcpy(p + 4, &cnt, 4); p += 8;
    for (int32_t i = 0; i < fs->count; i++) {
        uint32_t nl = (uint32_t) strlen(fs->files[i].name), dl = (uint32_t) fs->files[i].size;
        memcpy(p, &nl, 4); memcpy(p + 4, &dl, 4); p += 8;
        memcpy(p, fs->files[i].name, nl); p += nl;
        if (dl) memcpy(p, fs->files[i].data, dl);
        p += dl;
    }
    mz_ulong cl = mz_compressBound((mz_ulong) raw);
    uint8_t* comp = (uint8_t*) safeMalloc(cl + 4);
    uint32_t rawSize = (uint32_t) raw;
    memcpy(comp, &rawSize, 4);
    int zr = mz_compress2(comp + 4, &cl, buf, (mz_ulong) raw, 6);
    free(buf);
    if (zr != MZ_OK) { free(comp); return nullptr; }

    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof pkg);
    strncpy(pkg.desc_short, "UNDERTALE DC", sizeof pkg.desc_short);
    snprintf(pkg.desc_long, sizeof pkg.desc_long, "%s save data", fs->title);
    strncpy(pkg.app_id, "UTDC", sizeof pkg.app_id);
    uint8_t icon[512];
    buildIcon(&pkg, icon);
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.data_len = (int) (cl + 4);
    pkg.data = comp;
    uint8_t* out = nullptr;
    int outSize2 = 0;
    int r = vmu_pkg_build(&pkg, &out, &outSize2);
    free(comp);
    if (r < 0) return nullptr;
    *outSize = outSize2;
    return out;
}

bool DcFileSystem_flush(FileSystem* base) {
    DcFs* fs = (DcFs*) base;
    if (fs->writeFailed) { fs->writeFailed = false; fs->dirty = true; }
    if (!fs->dirty) return true;
    if (fs->busy) return false;            /* previous write still running; retry next tick */
    int size = 0;
    uint8_t* pkg = buildPackage(fs, &size);
    if (!pkg) return false;
    mutex_lock(&fs->jobLock);
    free(fs->jobData);
    fs->jobData = pkg;
    fs->jobSize = size;
    mutex_unlock(&fs->jobLock);
    fs->dirty = false;
    fs->busy = true;
    sem_signal(&fs->workSem);
    return true;
}

void DcFileSystem_waitIdle(FileSystem* base) {
    DcFs* fs = (DcFs*) base;
    while (fs->busy) thd_sleep(10);
}

static void loadFromVmu(DcFs* fs) {
    char path[64];
    if (!findVmu(path, sizeof path)) { snprintf(fs->status, sizeof fs->status, "No VMU found"); return; }
    file_t f = fs_open(path, O_RDONLY);
    if (f == FILEHND_INVALID) { snprintf(fs->status, sizeof fs->status, "No save on VMU"); return; }
    size_t n = fs_total(f);
    uint8_t* b = (uint8_t*) safeMalloc(n);
    ssize_t got = fs_read(f, b, n);
    fs_close(f);
    vmu_pkg_t pkg;
    if (got != (ssize_t) n || vmu_pkg_parse(b, n, &pkg) < 0 || pkg.data_len < 4) { free(b); snprintf(fs->status, sizeof fs->status, "VMU save is damaged"); return; }
    uint32_t rawSize;
    memcpy(&rawSize, pkg.data, 4);
    uint8_t* raw = (uint8_t*) safeMalloc(rawSize);
    mz_ulong rl = rawSize;
    if (mz_uncompress(raw, &rl, pkg.data + 4, (mz_ulong) (pkg.data_len - 4)) != MZ_OK || rl < 8) { free(raw); free(b); return; }
    free(b);
    uint8_t* p = raw;
    uint32_t magic, cnt;
    memcpy(&magic, p, 4); memcpy(&cnt, p + 4, 4); p += 8;
    if (magic != SAVE_MAGIC) { free(raw); return; }
    for (uint32_t i = 0; i < cnt && p + 8 <= raw + rl; i++) {
        uint32_t nl, dl;
        memcpy(&nl, p, 4); memcpy(&dl, p + 4, 4); p += 8;
        if (p + nl + dl > raw + rl) break;
        char name[256];
        uint32_t cn = nl < 255 ? nl : 255;
        memcpy(name, p, cn); name[cn] = 0; p += nl;
        MemFile* mf = createFile(fs, name);
        mf->data = (uint8_t*) safeMalloc(dl + 1);
        memcpy(mf->data, p, dl); mf->data[dl] = 0;
        mf->size = (int32_t) dl;
        p += dl;
    }
    free(raw);
    snprintf(fs->status, sizeof fs->status, "Loaded %d files from VMU", fs->count);
    logInfo("DC fs: %s\n", fs->status);
}

void DcFileSystem_tick(FileSystem* base) {
    DcFs* fs = (DcFs*) base;
    if (fs->writeFailed && !fs->busy) { fs->writeFailed = false; fs->dirty = true; fs->dirtySince = timer_ms_gettime64() + 5000; }
    if (!fs->dirty) return;
    if (timer_ms_gettime64() - fs->dirtySince < FLUSH_DELAY_MS) return;
    if (!DcFileSystem_flush(base)) fs->dirtySince = timer_ms_gettime64() + 5000; /* retry later */
}

bool DcFileSystem_hasUnsavedData(FileSystem* base) { return ((DcFs*) base)->unsaved; }
const char* DcFileSystem_status(FileSystem* base) { return ((DcFs*) base)->status; }

/* ------------------------------------------------------------------ */
/* Vtable                                                              */
/* ------------------------------------------------------------------ */

static char* dfResolvePath(MAYBE_UNUSED FileSystem* fs, const char* rel) { return normName(rel); }

static bool dfFileExists(FileSystem* base, const char* rel) {
    DcFs* fs = (DcFs*) base;
    if (findFile(fs, rel)) return true;
    uint8_t* d; int32_t n;
    if (readDisc(fs, rel, &d, &n)) { free(d); return true; }
    return false;
}

static bool dfReadFileBinary(FileSystem* base, const char* rel, uint8_t** out, int32_t* outSize) {
    DcFs* fs = (DcFs*) base;
    MemFile* f = findFile(fs, rel);
    if (f) {
        uint8_t* b = (uint8_t*) safeMalloc((size_t) f->size + 1);
        if (f->size) memcpy(b, f->data, (size_t) f->size);
        b[f->size] = 0;
        *out = b; *outSize = f->size;
        return true;
    }
    return readDisc(fs, rel, out, outSize);
}

static char* dfReadFileText(FileSystem* base, const char* rel) {
    uint8_t* d; int32_t n;
    if (!dfReadFileBinary(base, rel, &d, &n)) return nullptr;
    return (char*) d;
}

static bool dfWriteFileBinary(FileSystem* base, const char* rel, const uint8_t* data, int32_t size) {
    DcFs* fs = (DcFs*) base;
    MemFile* f = createFile(fs, rel);
    free(f->data);
    f->data = (uint8_t*) safeMalloc((size_t) size + 1);
    if (size) memcpy(f->data, data, (size_t) size);
    f->data[size] = 0;
    f->size = size;
    markDirty(fs);
    return true;
}

static bool dfWriteFileText(FileSystem* base, const char* rel, const char* contents) {
    return dfWriteFileBinary(base, rel, (const uint8_t*) contents, (int32_t) strlen(contents));
}

static bool dfDeleteFile(FileSystem* base, const char* rel) {
    DcFs* fs = (DcFs*) base;
    MemFile* f = findFile(fs, rel);
    if (!f) return false;
    int32_t idx = (int32_t) (f - fs->files);
    free(f->name); free(f->data);
    memmove(&fs->files[idx], &fs->files[idx + 1], (size_t) (fs->count - idx - 1) * sizeof(MemFile));
    fs->count--;
    markDirty(fs);
    return true;
}

static bool dfRenameFile(FileSystem* base, const char* oldRel, const char* newRel) {
    DcFs* fs = (DcFs*) base;
    MemFile* f = findFile(fs, oldRel);
    if (!f) return false;
    MemFile* existing = findFile(fs, newRel);
    if (existing && existing != f) { dfDeleteFile(base, newRel); f = findFile(fs, oldRel); }
    free(f->name);
    f->name = normName(newRel);
    markDirty(fs);
    return true;
}

static void* dfBinaryOpen(FileSystem* base, const char* rel, int32_t mode) {
    DcFs* fs = (DcFs*) base;
    MemHandle* h = (MemHandle*) safeCalloc(1, sizeof(MemHandle));
    h->mode = mode;
    if (mode == GML_FILE_BIN_READ) {
        MemFile* f = findFile(fs, rel);
        if (f) { h->name = normName(rel); h->hasFile = true; }
        else {
            if (!readDisc(fs, rel, &h->roData, &h->roSize)) { free(h); return nullptr; }
            h->readOnlyCopy = true;
        }
    } else {
        MemFile* f = createFile(fs, rel);
        if (mode == GML_FILE_BIN_WRITE) { free(f->data); f->data = nullptr; f->size = 0; }
        (void) f;
        h->name = normName(rel); h->hasFile = true;
        markDirty(fs);
    }
    return h;
}

static MemFile* handleFile(DcFs* fs, MemHandle* h) {
    if (h->readOnlyCopy || !h->hasFile) return nullptr;
    MemFile* f = findFile(fs, h->name);
    if (!f) f = createFile(fs, h->name);
    return f;
}

static void dfBinaryClose(MAYBE_UNUSED FileSystem* base, void* handle) {
    MemHandle* h = (MemHandle*) handle;
    if (!h) return;
    free(h->roData);
    free(h->name);
    free(h);
}

static int32_t dfBinaryRead(FileSystem* base, void* handle, void* dst, int32_t n) {
    MemHandle* h = (MemHandle*) handle;
    const uint8_t* data; int32_t size;
    if (h->readOnlyCopy) { data = h->roData; size = h->roSize; }
    else { MemFile* f = handleFile((DcFs*) base, h); data = f->data; size = f->size; }
    if (h->pos >= size) return 0;
    if (n > size - h->pos) n = size - h->pos;
    memcpy(dst, data + h->pos, (size_t) n);
    h->pos += n;
    return n;
}

static int32_t dfBinaryWrite(FileSystem* base, void* handle, const void* src, int32_t n) {
    MemHandle* h = (MemHandle*) handle;
    if (h->readOnlyCopy || h->mode == GML_FILE_BIN_READ) return 0;
    MemFile* f = handleFile((DcFs*) base, h);
    if (h->pos + n > f->size) {
        f->data = (uint8_t*) safeRealloc(f->data, (size_t) (h->pos + n + 1));
        if (h->pos > f->size) memset(f->data + f->size, 0, (size_t) (h->pos - f->size));
        f->size = h->pos + n;
        f->data[f->size] = 0;
    }
    memcpy(f->data + h->pos, src, (size_t) n);
    h->pos += n;
    markDirty((DcFs*) base);
    return n;
}

static int32_t dfBinaryTell(MAYBE_UNUSED FileSystem* base, void* handle) { return ((MemHandle*) handle)->pos; }
static bool dfBinarySeek(MAYBE_UNUSED FileSystem* base, void* handle, int32_t pos) { ((MemHandle*) handle)->pos = pos < 0 ? 0 : pos; return true; }
static int32_t dfBinarySize(FileSystem* base, void* handle) {
    MemHandle* h = (MemHandle*) handle;
    if (h->readOnlyCopy) return h->roSize;
    return handleFile((DcFs*) base, h)->size;
}
static void dfBinaryRewrite(FileSystem* base, void* handle) {
    MemHandle* h = (MemHandle*) handle;
    if (h->readOnlyCopy) return;
    MemFile* f = handleFile((DcFs*) base, h);
    free(f->data); f->data = nullptr; f->size = 0; h->pos = 0;
    markDirty((DcFs*) base);
}

static bool dfDirectoryExists(MAYBE_UNUSED FileSystem* fs, MAYBE_UNUSED const char* rel) { return true; }
static bool dfCreateDirectory(MAYBE_UNUSED FileSystem* fs, MAYBE_UNUSED const char* rel) { return true; }
static bool dfDeleteDirectory(MAYBE_UNUSED FileSystem* fs, MAYBE_UNUSED const char* rel) { return true; }

static FileSystemDirEntry* dfListDirectory(FileSystem* base, MAYBE_UNUSED const char* rel) {
    DcFs* fs = (DcFs*) base;
    FileSystemDirEntry* out = nullptr;
    for (int32_t i = 0; i < fs->count; i++) {
        FileSystemDirEntry e;
        e.name = safeStrdup(fs->files[i].name);
        e.isDirectory = false;
        arrput(out, e);
    }
    return out;
}

static FileSystemVtable dfVtable = {
    .resolvePath = dfResolvePath, .fileExists = dfFileExists, .readFileText = dfReadFileText,
    .writeFileText = dfWriteFileText, .deleteFile = dfDeleteFile, .readFileBinary = dfReadFileBinary,
    .writeFileBinary = dfWriteFileBinary, .renameFile = dfRenameFile,
    .binaryOpen = dfBinaryOpen, .binaryClose = dfBinaryClose, .binaryRead = dfBinaryRead, .binaryWrite = dfBinaryWrite,
    .binaryTell = dfBinaryTell, .binarySeek = dfBinarySeek, .binarySize = dfBinarySize, .binaryRewrite = dfBinaryRewrite,
    .directoryExists = dfDirectoryExists, .createDirectory = dfCreateDirectory, .deleteDirectory = dfDeleteDirectory,
    .listDirectory = dfListDirectory,
};

FileSystem* DcFileSystem_create(const char* gameTitle, const char* discRoot) {
    DcFs* fs = (DcFs*) safeCalloc(1, sizeof(DcFs));
    fs->base.vtable = &dfVtable;
    snprintf(fs->title, sizeof fs->title, "%s", gameTitle ? gameTitle : "Game");
    snprintf(fs->discRoot, sizeof fs->discRoot, "%s", discRoot);
    loadFromVmu(fs);
    sem_init(&fs->workSem, 0);
    mutex_init(&fs->jobLock, MUTEX_TYPE_NORMAL);
    fs->writer = thd_create(1, vmuWriter, fs);
    return &fs->base;
}
