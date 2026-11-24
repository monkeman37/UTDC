/* PowerVR2 VQ texture encoder (see vq.c).
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef UTDC_VQ_H
#define UTDC_VQ_H
#include <stdint.h>

/* px: size*size texels (row-major) already in the 16-bit page format (ARGB1555, or ARGB4444 when
   soft). size: power of two >= 8. out: 2048 + size*size/4 bytes. outErr (optional): mean squared
   error per 8-bit channel over opaque texels. Returns 0 on success. */
int vq_encode(const uint16_t* px, int size, int soft, uint8_t* out, double* outErr);

#endif
