/*
 * halftone.c - threshold-matrix halftoning (see halftone.h).
 *
 * The threshold tables themselves live in halftone_tables.h, which the build
 * generates (tools/gen_halftone.py) either from the "BRML" screen files of a
 * Brother driver package you already have, or from a built-in clustered-dot
 * screen when no such package is available.
 */
#include "halftone.h"

#include <string.h>

#include "halftone_tables.h"

const uint8_t *br_screen(br_screen_id id)
{
    if ((unsigned)id >= BR_SCREEN_COUNT)
        id = BR_SCREEN_600_GRAPHICS;
    return br_screen_tables[id];
}

const char *br_screen_source(void)
{
    return BR_SCREEN_SOURCE;
}

void br_halftone_row(const uint8_t *matrix, const uint8_t *src, int width,
                     int y, uint8_t *dst, int dst_bytes)
{
    const uint8_t *mrow = matrix + (y % BR_MATRIX_SIZE) * BR_MATRIX_SIZE;

    memset(dst, 0, (size_t)dst_bytes);

    for (int x = 0; x < width; x++) {
        const uint8_t px = src[x];
        if (px > mrow[x & (BR_MATRIX_SIZE - 1)] || px == 0xFF)
            dst[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }
}
