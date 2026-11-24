/*
 * halftone.h - threshold-matrix halftoning of 8-bit black (K) rows to 1 bit.
 */
#ifndef HL1210W_HALFTONE_H
#define HL1210W_HALFTONE_H

#include <stdint.h>

#define BR_MATRIX_SIZE 32 /* thresholds are tiled 32 x 32 */

typedef enum {
    BR_SCREEN_300_GRAPHICS = 0,
    BR_SCREEN_300_TEXT,
    BR_SCREEN_600_GRAPHICS,
    BR_SCREEN_600_TEXT,
    BR_SCREEN_1200_GRAPHICS,
    BR_SCREEN_1200_TEXT,
    BR_SCREEN_COUNT
} br_screen_id;

/* Returns the 32x32 threshold matrix for a screen (row major). */
const uint8_t *br_screen(br_screen_id id);

/* Human readable origin of the screens compiled into this binary. */
const char *br_screen_source(void);

/*
 * Halftone one row.
 *
 *   src     - 'width' pixels, 0 = white ... 255 = black
 *   y       - row number within the page (selects the matrix row)
 *   dst     - receives 'dst_bytes' bytes, MSB first, 1 = black; bits past
 *             'width' are cleared
 *
 * A pixel is printed when it is darker than the threshold; a fully black
 * pixel is always printed and a fully white one never is.
 */
void br_halftone_row(const uint8_t *matrix, const uint8_t *src, int width,
                     int y, uint8_t *dst, int dst_bytes);

#endif /* HL1210W_HALFTONE_H */
