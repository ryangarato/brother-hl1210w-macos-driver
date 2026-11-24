/*
 * smooth.c - 2x enlargement with stair-step smoothing (see smooth.h).
 *
 * Each source pixel normally becomes two identical destination pixels.  A
 * white pixel that sits in the inner corner of a black "L" (two orthogonal
 * neighbours black, the other two not) is where a diagonal edge steps; there
 * one half of the doubled pixel is painted black instead, and the fix is
 * carried along the edge for a number of rows that depends on how long the
 * step is in each direction.
 *
 * The thresholds and run-length rules reproduce the vendor driver so that the
 * result is pixel for pixel what the printer was tuned for.
 */
#include "smooth.h"

#define WHITE_MAX 4   /* a pixel this light is "white" for corner detection */
#define BLACK_MIN 254 /* a pixel this dark is "black"                       */
#define LIGHT_MAX 14  /* a pixel this light still counts as background      */

int br_smooth_width(int width)
{
    return (2 * width + 3) & ~3;
}

/* How many rows a corner fix is carried, growing away from the long side. */
static int carry_toward_long(int near_run, int far_run)
{
    /* near_run: step length on the side we write towards. */
    if (near_run < 2 || far_run < 2)
        return (near_run == 1 || far_run == 1) ? 1 : 0;
    if (far_run < near_run)
        return far_run - 1;
    return (near_run + 1) / 2;
}

void br_smooth_expand_x(const uint8_t *src, size_t src_stride, int width,
                        int height, uint8_t *dst, size_t dst_stride)
{
    const ptrdiff_t S = (ptrdiff_t)src_stride;
    const ptrdiff_t D = (ptrdiff_t)dst_stride;

    for (int y = 0; y < height; y++) {
        const uint8_t *row = src + (ptrdiff_t)y * S;
        uint8_t       *out = dst + (ptrdiff_t)y * D;

        for (int x = 0; x < width; x++) {
            const uint8_t *c  = row + x; /* centre pixel            */
            uint8_t       *o  = out + 2 * (ptrdiff_t)x;
            int            carried = 0;

            if (y >= 1 && y < height - 1 && x >= 1 && x < width - 1 &&
                c[0] <= WHITE_MAX) {
                const unsigned mask =
                    (c[-S] >= BLACK_MIN ? 1u : 0u) | /* up    */
                    (c[-1] >= BLACK_MIN ? 2u : 0u) | /* left  */
                    (c[S] >= BLACK_MIN ? 4u : 0u) |  /* down  */
                    (c[1] >= BLACK_MIN ? 8u : 0u);   /* right */
                int down, up, j;

                switch (mask) {
                case 3: /* black above and to the left */
                    if (c[S - 1] < BLACK_MIN || c[-S + 1] > WHITE_MAX)
                        break;

                    /* Rows below that continue the vertical edge on the left. */
                    for (j = 1;; j++) {
                        if (y + j >= height)
                            break;
                        const uint8_t *r = c + j * S;
                        if (r[-1] < BLACK_MIN || r[0] > LIGHT_MAX ||
                            r[1] > LIGHT_MAX)
                            break;
                    }
                    down = j - 1;

                    /* Rows above where the black keeps its right-hand edge. */
                    for (j = 0; y - 1 - j > 0; j++) {
                        const uint8_t *r = c - (j + 1) * S;
                        if (r[1] > LIGHT_MAX || r[0] < BLACK_MIN)
                            break;
                    }
                    up = j;

                    carried = carry_toward_long(down, up);
                    for (j = 0; j < carried; j++) {
                        o[j * D]     = c[-1];
                        o[j * D + 1] = c[0];
                    }
                    break;

                case 6: /* black to the left and below */
                    if (c[-S - 1] < BLACK_MIN || c[S + 1] > WHITE_MAX)
                        break;

                    for (j = 0; y - 1 - j >= 1; j++) {
                        const uint8_t *r = c - (j + 1) * S;
                        if (r[-1] < BLACK_MIN || r[0] > LIGHT_MAX ||
                            r[1] > LIGHT_MAX)
                            break;
                    }
                    up = j;

                    for (j = 1;; j++) {
                        if (y + j >= height)
                            break;
                        const uint8_t *r = c + j * S;
                        if (r[1] > LIGHT_MAX || r[0] < BLACK_MIN)
                            break;
                    }
                    down = j - 1;

                    carried = carry_toward_long(up, down);
                    for (j = 0; j < carried; j++) {
                        o[-j * D]     = c[-1];
                        o[-j * D + 1] = c[0];
                    }
                    break;

                case 9: /* black above and to the right */
                    if (c[S + 1] < BLACK_MIN || c[-S - 1] > WHITE_MAX)
                        break;

                    for (j = 1; y + j < height; j++) {
                        const uint8_t *r = c + j * S;
                        if (r[-1] > LIGHT_MAX || r[0] > LIGHT_MAX ||
                            r[1] < BLACK_MIN)
                            break;
                    }
                    down = j - 1;

                    for (j = 0; y - 1 - j > 0; j++) {
                        const uint8_t *r = c - (j + 1) * S;
                        if (r[-1] > LIGHT_MAX || r[0] < BLACK_MIN)
                            break;
                    }
                    up = j;

                    carried = carry_toward_long(down, up);
                    for (j = 0; j < carried; j++) {
                        o[j * D]     = c[0];
                        o[j * D + 1] = c[1];
                    }
                    break;

                case 12: /* black below and to the right */
                    if (c[-S + 1] < BLACK_MIN || c[S - 1] > WHITE_MAX)
                        break;

                    for (j = 0; y - 1 - j >= 1; j++) {
                        const uint8_t *r = c - (j + 1) * S;
                        if (r[-1] > LIGHT_MAX || r[0] > LIGHT_MAX ||
                            r[1] < BLACK_MIN)
                            break;
                    }
                    up = j;

                    for (j = 1;; j++) {
                        if (y + j >= height)
                            break;
                        const uint8_t *r = c + j * S;
                        if (r[-1] > LIGHT_MAX || r[0] < BLACK_MIN)
                            break;
                    }
                    down = j - 1;

                    carried = carry_toward_long(up, down);
                    for (j = 0; j < carried; j++) {
                        o[-j * D]     = c[0];
                        o[-j * D + 1] = c[1];
                    }
                    break;

                default:
                    break;
                }
            }

            if (carried == 0) {
                /* Plain doubling, without undoing an earlier corner fix. */
                if (o[0] == 0)
                    o[0] = c[0];
                if (o[1] == 0)
                    o[1] = c[0];
            }
        }
    }
}
