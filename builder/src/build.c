/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "build.h"
#include "datawin.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define RMDIR(p) rmdir(p)
#define MKDIR(p) mkdir(p, 0755)
#endif

void build_log(BuildContext* ctx, const char* fmt, ...) {
    char buf[1024];
    va_list va; va_start(va, fmt); vsnprintf(buf, sizeof buf, fmt, va); va_end(va);
    if (ctx->log) ctx->log(ctx->user, buf);
}

void build_progress(BuildContext* ctx, double fraction, const char* fmt, ...) {
    char buf[512];
    va_list va; va_start(va, fmt); vsnprintf(buf, sizeof buf, fmt, va); va_end(va);
    if (ctx->progress) ctx->progress(ctx->user, fraction, buf);
}

int build_cancelled(BuildContext* ctx) { return ctx->cancel; }

static void dir_of(const char* path, char* out, size_t n) {
    const char* slash = NULL;
    for (const char* c = path; *c; c++) if (*c == '/' || *c == '\\') slash = c;
    if (slash) snprintf(out, n, "%.*s", (int)(slash - path), path);
    else snprintf(out, n, ".");
}

int utdc_build(BuildContext* ctx, const BuildOptions* opts) {
    char path[1024], tmp[1024], err[256];
    int rc = -1;
    DataWinFile dw;

    build_progress(ctx, 0.0, "Reading data.win");
    snprintf(path, sizeof path, "%s/data.win", opts->gameDir);
    if (dw_load(&dw, path, err, sizeof err)) {
        build_log(ctx, "error: %s", err);
        build_log(ctx, "Select the folder that contains UNDERTALE.exe and data.win.");
        return -1;
    }
    build_log(ctx, "Found \"%s\" (bytecode %u): %u sprites pages, %u texture items, %u sounds",
              dw.displayName, dw.wadVersion, dw.texCount, dw.tpagCount, dw.soundCount);
    if (dw.wadVersion < 13 || dw.wadVersion > 17) {
        build_log(ctx, "error: unsupported GameMaker bytecode version %u", dw.wadVersion);
        goto out;
    }

    if (opts->tempDir) snprintf(tmp, sizeof tmp, "%s", opts->tempDir);
    else { char d[1024]; dir_of(opts->outPath, d, sizeof d); snprintf(tmp, sizeof tmp, "%s/utdc_tmp", d); }
    MKDIR(tmp);

    char texPath[1100], audPath[1100], dwPath[1100], cfgPath[1100];
    snprintf(texPath, sizeof texPath, "%s/TEXTURES.BIN", tmp);
    snprintf(audPath, sizeof audPath, "%s/AUDIO.BIN", tmp);
    snprintf(dwPath, sizeof dwPath, "%s/DATA.WIN", tmp);
    snprintf(cfgPath, sizeof cfgPath, "%s/UTDC.CFG", tmp);

    build_progress(ctx, 0.02, "Writing game code");
    if (dw_write_stripped(&dw, dwPath)) { build_log(ctx, "error: could not write %s", dwPath); goto out; }
    if (build_textures(ctx, &dw, texPath)) goto out;
    if (build_audio(ctx, &dw, opts, audPath)) goto out;

    {
        FILE* f = fopen(cfgPath, "wb");
        if (!f) goto out;
        fprintf(f, "title=%s\nbuilder=utdc %s\n", dw.displayName, UTDC_VERSION);
        if (opts->overlay) fprintf(f, "overlay=1\n");
        if (opts->exitFrame > 0) fprintf(f, "exitframe=%d\n", opts->exitFrame);
        fclose(f);
    }

    {
        DiscFile files[] = {
            { "1ST_READ.BIN", NULL, utdc_payload_1st_read, utdc_payload_1st_read_size },
            { "DATA.WIN", dwPath, NULL, 0 },
            { "TEXTURES.BIN", texPath, NULL, 0 },
            { "AUDIO.BIN", audPath, NULL, 0 },
            { "UTDC.CFG", cfgPath, NULL, 0 },
            { "INPUT.JSN", opts->testInputs, NULL, 0 },
        };
        int fileCount = (int)(sizeof files / sizeof files[0]);
        if (!opts->testInputs) fileCount--;
        if (utdc_payload_1st_read_size < 1024) build_log(ctx, "warning: this builder has no Dreamcast runner embedded (development build)");
        if (disc_write(ctx, opts->format, opts->outPath, utdc_payload_ip_bin, utdc_payload_ip_bin_size,
                       files, fileCount, "UNDERTALE_DC")) goto out;
    }
    build_progress(ctx, 1.0, "Done");
    rc = 0;
out:
    remove(texPath); remove(audPath); remove(dwPath); remove(cfgPath);
    RMDIR(tmp);
    dw_free(&dw);
    return rc;
}
