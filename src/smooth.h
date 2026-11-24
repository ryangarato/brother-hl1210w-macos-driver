/*
 * smooth.h - 2x enlargement with stair-step smoothing.
 *
 * This is the "600 dpi from a 300 dpi rendering" mode of the vendor driver:
 * every pixel is doubled horizontally and vertically, and the inner corners
 * of diagonal black edges are filled in so that slanted strokes do not look
 * like a staircase.
 */
#ifndef HL1210W_SMOOTH_H
#define HL1210W_SMOOTH_H

#include <stddef.h>
#include <stdint.h>

/* Width in pixels of the enlarged image: twice 'width', rounded up to 4. */
int br_smooth_width(int width);

/*
 * Double an 8-bit black (K) image horizontally with edge smoothing.
 *
 *   src        - 'height' rows of 'width' pixels, 'src_stride' bytes apart
 *   dst        - 'height' rows of br_smooth_width(width) bytes; must be
 *                zero filled by the caller
 *   dst_stride - br_smooth_width(width)
 *
 * Rows still have to be doubled vertically afterwards.
 */
void br_smooth_expand_x(const uint8_t *src, size_t src_stride, int width,
                        int height, uint8_t *dst, size_t dst_stride);

#endif /* HL1210W_SMOOTH_H */
