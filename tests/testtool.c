/*
 * hl1210w-testtool - exposes the driver's building blocks to the test suite.
 *
 *   page    <native|smooth> W H MARGIN MATRIX IN OUT
 *           8-bit black page (W*H bytes) -> compressed raster blocks
 *   smooth  W H IN OUT
 *           horizontal 2x smoothing only -> br_smooth_width(W) * H bytes
 *   margin  LEFT_PT EXTRA_PT DPI      -> prints the left margin in bytes
 *   paper   WIDTH_PT LENGTH_PT        -> prints "<PJL name> <extra points>"
 *   header  RES TEXT TONER IMPROVE DENSITY MEDIA WIDTH_PT LENGTH_PT EPOCH USER TITLE
 *           -> job header on stdout (DENSITY 99 = printer default)
 *   screen  ID OUT                    -> writes a compiled-in 32x32 screen
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halftone.h"
#include "job.h"
#include "page.h"
#include "smooth.h"

static uint8_t *read_file(const char *path, size_t want)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    uint8_t *buf = malloc(want ? want : 1);
    if (!buf || fread(buf, 1, want, f) != want) {
        fprintf(stderr, "%s: short read (wanted %zu bytes)\n", path, want);
        exit(2);
    }
    fclose(f);
    return buf;
}

static FILE *open_out(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    return f;
}

static int usage(void)
{
    fputs("usage: hl1210w-testtool page|smooth|margin|paper|header|screen ...\n",
          stderr);
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();

    if (!strcmp(argv[1], "page") && argc == 9) {
        const bool smooth = !strcmp(argv[2], "smooth");
        const int  w      = atoi(argv[3]);
        const int  h      = atoi(argv[4]);
        const int  margin = atoi(argv[5]);
        uint8_t   *matrix = read_file(argv[6], BR_MATRIX_SIZE * BR_MATRIX_SIZE);
        uint8_t   *px     = read_file(argv[7], (size_t)w * (size_t)h);
        FILE      *out    = open_out(argv[8]);

        int rc = br_send_page(out, px, (size_t)w, w, h, smooth, matrix, margin,
                              NULL);
        fclose(out);
        return rc ? 1 : 0;
    }

    if (!strcmp(argv[1], "smooth") && argc == 6) {
        const int w  = atoi(argv[2]);
        const int h  = atoi(argv[3]);
        const int ew = br_smooth_width(w);
        uint8_t  *px = read_file(argv[4], (size_t)w * (size_t)h);
        uint8_t  *wide = calloc((size_t)h, (size_t)ew);
        FILE     *out  = open_out(argv[5]);

        br_smooth_expand_x(px, (size_t)w, w, h, wide, (size_t)ew);
        fwrite(wide, 1, (size_t)ew * (size_t)h, out);
        fclose(out);
        return 0;
    }

    if (!strcmp(argv[1], "margin") && argc == 5) {
        printf("%d\n", br_left_margin_bytes((unsigned)atoi(argv[2]),
                                            atoi(argv[3]), atoi(argv[4])));
        return 0;
    }

    if (!strcmp(argv[1], "paper") && argc == 4) {
        int         extra = 0;
        const char *name  = br_paper_for_size((unsigned)atoi(argv[2]),
                                              (unsigned)atoi(argv[3]), &extra);
        printf("%s %d\n", name, extra);
        return 0;
    }

    if (!strcmp(argv[1], "header") && argc == 13) {
        br_job job;
        memset(&job, 0, sizeof(job));
        job.resolution  = (br_resolution)atoi(argv[2]);
        job.text_screen = atoi(argv[3]) != 0;
        job.toner_save  = atoi(argv[4]) != 0;
        job.improve     = (br_improve)atoi(argv[5]);
        if (atoi(argv[6]) != 99) {
            job.density_set = true;
            job.density     = atoi(argv[6]);
        }
        job.media_type     = (unsigned)atoi(argv[7]);
        job.page_width_pt  = (unsigned)atoi(argv[8]);
        job.page_length_pt = (unsigned)atoi(argv[9]);
        job.left_margin_pt = 12;
        job.now            = (time_t)atol(argv[10]);
        br_pjl_string(job.user, sizeof(job.user), argv[11]);
        br_pjl_string(job.title, sizeof(job.title), argv[12]);

        br_job_begin(stdout, &job);
        br_page_header(stdout, true, 1);
        br_page_trailer(stdout);
        br_job_end(stdout, &job);
        return 0;
    }

    if (!strcmp(argv[1], "screen") && argc == 4) {
        FILE *out = open_out(argv[3]);
        fwrite(br_screen((br_screen_id)atoi(argv[2])), 1,
               BR_MATRIX_SIZE * BR_MATRIX_SIZE, out);
        fclose(out);
        return 0;
    }

    return usage();
}
