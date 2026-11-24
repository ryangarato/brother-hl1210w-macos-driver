/*
 * page.c - turn one 8-bit black (K) page into compressed raster blocks.
 */
#include "page.h"

#include <stdlib.h>

#include "brcomp.h"
#include "halftone.h"
#include "smooth.h"

int br_send_page(FILE *out, const uint8_t *px, size_t stride, int width,
                 int height, bool smooth, const uint8_t *matrix, int margin,
                 const volatile sig_atomic_t *cancel)
{
    const int out_width = smooth ? br_smooth_width(width) : width;
    const int row_bytes = (out_width + 7) / 8;

    uint8_t *wide = NULL; /* horizontally doubled copy for smoothing */
    uint8_t *bits = malloc((size_t)row_bytes);
    br_page  pg;

    if (!bits)
        return -1;

    if (smooth) {
        wide = calloc((size_t)height, (size_t)out_width);
        if (!wide) {
            free(bits);
            return -1;
        }
        br_smooth_expand_x(px, stride, width, height, wide, (size_t)out_width);
    }

    if (br_page_begin(&pg, out, row_bytes, margin) != 0) {
        free(wide);
        free(bits);
        return -1;
    }

    for (int y = 0; y < height; y++) {
        if (cancel && *cancel)
            break;

        if (smooth) {
            /* Printing each smoothed row twice is the vertical doubling. */
            const uint8_t *row = wide + (size_t)y * (size_t)out_width;
            for (int half = 0; half < 2; half++) {
                br_halftone_row(matrix, row, out_width, 2 * y + half, bits,
                                row_bytes);
                br_page_add_row(&pg, bits);
            }
        } else {
            br_halftone_row(matrix, px + (size_t)y * stride, width, y, bits,
                            row_bytes);
            br_page_add_row(&pg, bits);
        }
    }

    const int rc = br_page_end(&pg);
    free(wide);
    free(bits);
    return rc;
}
