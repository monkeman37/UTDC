/* Command-line front end: utdc-builder <game folder> <output.gdi>
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "build.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_log(void* u, const char* line) { (void)u; printf("%s\n", line); fflush(stdout); }
static void on_progress(void* u, double f, const char* s) {
    (void)u;
    static int last = -1;
    int pct = (int)(f * 100);
    if (pct != last) { printf("[%3d%%] %s\n", pct, s); fflush(stdout); last = pct; }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "UTDC builder %s\nusage: %s <Undertale folder> <output.gdi> [--music-rate HZ] [--sfx-rate HZ] [--overlay] [--test-inputs FILE] [--exit-frame N]\n", UTDC_VERSION, argv[0]);
        return 2;
    }
    BuildOptions o = {0};
    o.gameDir = argv[1];
    o.outPath = argv[2];
    o.format = DISC_FORMAT_GDI;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--overlay")) { o.overlay = 1; continue; }
        if (i + 1 >= argc) break;
        if (!strcmp(argv[i], "--music-rate")) o.musicRate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sfx-rate")) o.sfxRate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--test-inputs")) o.testInputs = argv[++i];
        else if (!strcmp(argv[i], "--exit-frame")) o.exitFrame = atoi(argv[++i]);
    }
    BuildContext ctx = {0};
    ctx.log = on_log; ctx.progress = on_progress;
    int rc = utdc_build(&ctx, &o);
    if (rc == 0) printf("Build finished.\n");
    return rc ? 1 : 0;
}
