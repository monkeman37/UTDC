/* UTDC ROM builder - shared build pipeline API.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef UTDC_BUILD_H
#define UTDC_BUILD_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    DISC_FORMAT_GDI = 0,   /* GD-ROM image: emulators (Redream, Flycast) and GDEMU/ODEs */
    DISC_FORMAT_CDI = 1    /* DiscJuggler image: burn to CD-R for real hardware */
} DiscFormat;

typedef struct {
    const char* gameDir;     /* folder containing data.win and the .ogg files */
    const char* outPath;     /* .gdi or .cdi path */
    DiscFormat  format;
    int         musicRate;   /* Hz for streamed music, default 44100 (same as the PC version) */
    int         sfxRate;     /* max Hz for sound effects, default 44100 */
    const char* tempDir;     /* scratch folder; defaults next to outPath */
    /* developer/test options */
    const char* testInputs;  /* Butterscotch input-playback JSON copied to the disc as INPUT.JSN */
    int         overlay;     /* show the fps/timing overlay */
    int         exitFrame;   /* quit to BIOS after this game frame (0 = never) */
} BuildOptions;

typedef void (*BuildLogFn)(void* user, const char* line);
typedef void (*BuildProgressFn)(void* user, double fraction, const char* status);

typedef struct {
    int texPages;
    int sfxCount;
    int streamCount;
    double musicSeconds;
    uint64_t discBytes;
} BuildStats;

typedef struct BuildContext {
    BuildLogFn log;
    BuildProgressFn progress;
    void* user;
    volatile int cancel;
    BuildStats stats;
} BuildContext;

/* Runs the whole pipeline. Returns 0 on success. */
int utdc_build(BuildContext* ctx, const BuildOptions* opts);

/* internals */
struct DataWinFile_;
void build_log(BuildContext* ctx, const char* fmt, ...);
void build_progress(BuildContext* ctx, double fraction, const char* fmt, ...);
int  build_cancelled(BuildContext* ctx);

#include "datawin.h"
int build_textures(BuildContext* ctx, const DataWinFile* dw, const char* outPath);
int build_audio(BuildContext* ctx, const DataWinFile* dw, const BuildOptions* opts, const char* outPath);

/* Disc image writer */
typedef struct {
    const char* isoName;   /* ISO9660 name, e.g. "DATA.WIN" */
    const char* srcPath;   /* host file */
    const uint8_t* mem;    /* or in-memory contents */
    uint32_t memSize;
} DiscFile;

int disc_write(BuildContext* ctx, DiscFormat fmt, const char* outPath,
               const uint8_t* ipBin, size_t ipSize,
               const DiscFile* files, int fileCount, const char* volumeName);

/* Payload embedded into the builder at compile time (see payload/) */
extern const uint8_t utdc_payload_1st_read[];
extern const uint32_t utdc_payload_1st_read_size;
extern const uint8_t utdc_payload_ip_bin[];
extern const uint32_t utdc_payload_ip_bin_size;

#endif
