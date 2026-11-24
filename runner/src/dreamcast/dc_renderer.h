/* PowerVR2 renderer for Butterscotch on the Sega Dreamcast.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef _BS_DC_RENDERER_H_
#define _BS_DC_RENDERER_H_

#include "renderer.h"

typedef struct {
    uint32_t pagesLoaded;      /* pages currently resident in VRAM */
    uint32_t pageLoadsFrame;   /* page uploads during the last frame */
    uint32_t pageLoadsTotal;
    uint32_t evictions;
    uint32_t polysFrame;       /* polygons submitted during the last frame */
    uint32_t vramFree;
    uint32_t loadUsTotal;      /* time spent loading pages (disc + inflate + upload) */
    uint32_t loadUsMax;
} DcRendererStats;

/* texturesPath: path of TEXTURES.BIN produced by the builder (e.g. "/cd/TEXTURES.BIN") */
Renderer* DcRenderer_create(const char* texturesPath);

/* Called by the platform loop around Runner_draw*. */
void DcRenderer_beginScene(Renderer* renderer);
void DcRenderer_endScene(Renderer* renderer);
void DcRenderer_beginNullScene(Renderer* renderer);
void DcRenderer_probeFramebuffer(Renderer* renderer);
void DcRenderer_setWindowOffset(Renderer* renderer, float dx, float dy);

/* Draws a solid rectangle in screen space (debug overlay / loading screen helpers). */
void DcRenderer_screenRect(Renderer* renderer, float x0, float y0, float x1, float y1, uint32_t argb);

const DcRendererStats* DcRenderer_stats(Renderer* renderer);

#endif
