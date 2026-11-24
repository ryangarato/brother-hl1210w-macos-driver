/*
 * page.h - turn one 8-bit black (K) page into compressed raster blocks.
 */
#ifndef HL1210W_PAGE_H
#define HL1210W_PAGE_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Halftone, compress and write the raster data of one page (everything
 * between "ESC*b1030m" and the form feed).
 *
 *   px      - 'height' rows of 'width' pixels, 'stride' bytes apart;
 *             0 = white, 255 = black
 *   smooth  - double the page in both directions with edge smoothing
 *             (a 300 dpi rendering printed at 600 dpi)
 *   matrix  - 32 x 32 halftone thresholds for the output resolution
 *   margin  - white bytes the printer inserts ahead of every row
 *   cancel  - optional flag; when it becomes non-zero the page is cut short
 *
 * Returns 0 on success, -1 on a write error or when memory runs out.
 */
int br_send_page(FILE *out, const uint8_t *px, size_t stride, int width,
                 int height, bool smooth, const uint8_t *matrix, int margin,
                 const volatile sig_atomic_t *cancel);

#endif /* HL1210W_PAGE_H */
