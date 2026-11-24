/*
 * rastertohl1210w - CUPS raster filter for the Brother HL-1210W series.
 *
 * Usage (run by cupsd):  rastertohl1210w job-id user title copies options [file]
 *
 * Reads CUPS raster (8-bit black) on stdin or from 'file' and writes the
 * printer's PJL + compressed raster data stream on stdout.
 *
 * Driver settings travel in the raster page header, where the RIP places
 * them from the PPD (see ppd/HL1210W.ppd):
 *
 *   HWResolution     300, 600 or 1200 dpi rendering
 *   cupsMediaType    1 = plain, 10 = recycled, ...
 *   cupsInteger0     1 = smooth a 300 dpi rendering up to 600 dpi
 *   cupsInteger1     halftone screen: 0 = graphics, 1 = text
 *   cupsInteger2     1 = toner save
 *   cupsInteger3     improve output: 0 = off, 1 = reduce curl, 2 = toner fixing
 *   cupsInteger4     1 = skip blank pages
 *   cupsInteger5     0 = printer's density, else density + 100 (94 .. 106)
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cups/cups.h>
#include <cups/raster.h>

#include "halftone.h"
#include "job.h"
#include "page.h"

#ifndef HL1210W_VERSION
#define HL1210W_VERSION "1.0"
#endif

#define MAX_PAGE_DIMENSION 32767 /* generous: Legal at 1200 dpi is 16800 */

static volatile sig_atomic_t g_cancelled;

static void on_sigterm(int sig)
{
    (void)sig;
    g_cancelled = 1;
}

static bool page_is_blank(const uint8_t *px, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (px[i])
            return false;
    return true;
}

/* Validate a page header and work out the resolution mode it asks for. */
static bool resolution_for_header(const cups_page_header2_t *h,
                                  br_resolution *res)
{
    if (h->HWResolution[0] != h->HWResolution[1])
        return false;

    switch (h->HWResolution[0]) {
    case 300:
        *res = h->cupsInteger[0] == 1 ? BR_RES_600_SMOOTH : BR_RES_300;
        return true;
    case 600:
        *res = BR_RES_600;
        return true;
    case 1200:
        *res = BR_RES_1200;
        return true;
    default:
        return false;
    }
}

static br_screen_id screen_for(br_resolution res, bool text)
{
    switch (res) {
    case BR_RES_300:
        return text ? BR_SCREEN_300_TEXT : BR_SCREEN_300_GRAPHICS;
    case BR_RES_1200:
        return text ? BR_SCREEN_1200_TEXT : BR_SCREEN_1200_GRAPHICS;
    case BR_RES_600:
    case BR_RES_600_SMOOTH:
    default:
        return text ? BR_SCREEN_600_TEXT : BR_SCREEN_600_GRAPHICS;
    }
}

static void job_from_header(br_job *job, const cups_page_header2_t *h,
                            br_resolution res, const char *user,
                            const char *title)
{
    memset(job, 0, sizeof(*job));

    br_pjl_string(job->user, sizeof(job->user), user);
    br_pjl_string(job->title, sizeof(job->title), title);

    job->resolution  = res;
    job->text_screen = h->cupsInteger[1] == 1;
    job->toner_save  = h->cupsInteger[2] == 1;
    job->improve     = h->cupsInteger[3] == 1   ? BR_IMPROVE_REDUCE_CURL
                       : h->cupsInteger[3] == 2 ? BR_IMPROVE_TONER_FIXING
                                                : BR_IMPROVE_OFF;
    job->skip_blank  = h->cupsInteger[4] == 1;

    if (h->cupsInteger[5] >= 94 && h->cupsInteger[5] <= 106) {
        job->density_set = true;
        job->density     = (int)h->cupsInteger[5] - 100;
    }

    job->media_type     = h->cupsMediaType;
    job->page_width_pt  = h->PageSize[0];
    job->page_length_pt = h->PageSize[1];
    job->left_margin_pt = h->Margins[0];
    job->now            = time(NULL);
}

int main(int argc, char *argv[])
{
    int                 fd = STDIN_FILENO;
    cups_raster_t      *ras;
    cups_page_header2_t header;
    br_job              job;
    bool                job_open      = false;
    unsigned            page          = 0;
    unsigned            pages_printed = 0;
    int                 status        = 0;
    uint8_t            *pixels        = NULL;
    size_t              pixels_size   = 0;

    setbuf(stderr, NULL);

    if (argc < 6 || argc > 7) {
        fprintf(stderr,
                "Usage: %s job-id user title copies options [file]\n",
                argv[0] ? argv[0] : "rastertohl1210w");
        return 1;
    }

    /* cupsd sends SIGTERM to cancel a job; finish the stream cleanly. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigterm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (argc == 7) {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "ERROR: Unable to open raster file \"%s\": %s\n",
                    argv[6], strerror(errno));
            return 1;
        }
    }

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras) {
        fprintf(stderr, "ERROR: Unable to read the raster stream: %s\n",
                cupsRasterErrorString());
        if (fd != STDIN_FILENO)
            close(fd);
        return 1;
    }

    fprintf(stderr, "DEBUG: rastertohl1210w %s, halftone screens: %s\n",
            HL1210W_VERSION, br_screen_source());

    while (!g_cancelled && cupsRasterReadHeader2(ras, &header)) {
        br_resolution res;

        page++;

        fprintf(stderr,
                "DEBUG: page %u: %ux%u px, %ux%u dpi, %u bpc, %u bpp, "
                "colorspace %u, %u bytes/line, media type %u\n",
                page, header.cupsWidth, header.cupsHeight,
                header.HWResolution[0], header.HWResolution[1],
                header.cupsBitsPerColor, header.cupsBitsPerPixel,
                (unsigned)header.cupsColorSpace, header.cupsBytesPerLine,
                header.cupsMediaType);

        if (header.cupsBitsPerColor != 8 || header.cupsBitsPerPixel != 8 ||
            (header.cupsColorSpace != CUPS_CSPACE_K &&
             header.cupsColorSpace != CUPS_CSPACE_W &&
             header.cupsColorSpace != CUPS_CSPACE_SW)) {
            fputs("ERROR: Unsupported raster format (need 8-bit grayscale).\n",
                  stderr);
            status = 1;
            break;
        }

        if (header.cupsWidth == 0 || header.cupsHeight == 0 ||
            header.cupsWidth > MAX_PAGE_DIMENSION ||
            header.cupsHeight > MAX_PAGE_DIMENSION ||
            header.cupsBytesPerLine < header.cupsWidth ||
            header.cupsBytesPerLine > MAX_PAGE_DIMENSION) {
            fputs("ERROR: Unsupported page dimensions.\n", stderr);
            status = 1;
            break;
        }

        if (!resolution_for_header(&header, &res)) {
            fprintf(stderr, "ERROR: Unsupported resolution %ux%u dpi.\n",
                    header.HWResolution[0], header.HWResolution[1]);
            status = 1;
            break;
        }

        if (!job_open) {
            job_from_header(&job, &header, res, argv[2], argv[3]);
            if (br_job_begin(stdout, &job) != 0) {
                status = 1;
                break;
            }
            job_open = true;
        } else if (res != job.resolution) {
            fputs("ERROR: The resolution changed in the middle of the job.\n",
                  stderr);
            status = 1;
            break;
        }

        /* Read the whole page. */
        const size_t stride = header.cupsBytesPerLine;
        const size_t need   = stride * header.cupsHeight;

        if (need > pixels_size) {
            uint8_t *bigger = realloc(pixels, need);
            if (!bigger) {
                fputs("ERROR: Not enough memory to hold the page.\n", stderr);
                status = 1;
                break;
            }
            pixels      = bigger;
            pixels_size = need;
        }

        bool truncated = false;
        for (unsigned y = 0; y < header.cupsHeight && !g_cancelled; y++) {
            if (cupsRasterReadPixels(ras, pixels + (size_t)y * stride,
                                     (unsigned)stride) != stride) {
                truncated = true;
                break;
            }
        }
        if (g_cancelled)
            break;
        if (truncated) {
            fputs("ERROR: The raster stream ended in the middle of a page.\n",
                  stderr);
            status = 1;
            break;
        }

        /* White-is-255 input: flip it so that 255 means black. */
        if (header.cupsColorSpace != CUPS_CSPACE_K) {
            for (size_t i = 0; i < need; i++)
                pixels[i] = (uint8_t)~pixels[i];
        }

        if (job.skip_blank && page_is_blank(pixels, need)) {
            fprintf(stderr, "INFO: Skipping blank page %u.\n", page);
            continue;
        }

        fprintf(stderr, "INFO: Printing page %u.\n", page);

        /* The printer makes the copies itself (*cupsManualCopies: False). */
        const unsigned copies = header.NumCopies < 1     ? 1
                                : header.NumCopies > 999 ? 999
                                                         : header.NumCopies;

        int extra_pt = 0;
        (void)br_paper_for_size(header.PageSize[0], header.PageSize[1],
                                &extra_pt);
        const int margin = br_left_margin_bytes(
            header.Margins[0], extra_pt, br_output_dpi(job.resolution));
        const uint8_t *matrix =
            br_screen(screen_for(job.resolution, job.text_screen));

        if (br_page_header(stdout, pages_printed == 0, copies) != 0 ||
            br_send_page(stdout, pixels, stride, (int)header.cupsWidth,
                         (int)header.cupsHeight,
                         job.resolution == BR_RES_600_SMOOTH, matrix, margin,
                         &g_cancelled) != 0) {
            fputs("ERROR: Unable to process or send the page.\n", stderr);
            status = 1;
            break;
        }

        if (g_cancelled) {
            /* Let the printer finish the block it was promised. */
            br_job_abort_padding(stdout);
            break;
        }

        if (br_page_trailer(stdout) != 0 || fflush(stdout) != 0) {
            status = 1;
            break;
        }

        pages_printed++;
        fprintf(stderr, "PAGE: %u %u\n", page, copies);
    }

    if (job_open) {
        if (br_job_end(stdout, &job) != 0)
            status = 1;
    }

    if (status != 0 && !g_cancelled && ferror(stdout))
        fputs("ERROR: Unable to send print data to the printer.\n", stderr);

    free(pixels);
    cupsRasterClose(ras);
    if (fd != STDIN_FILENO)
        close(fd);

    if (g_cancelled) {
        fputs("INFO: Job cancelled.\n", stderr);
        return 0;
    }

    if (page == 0 && status == 0) {
        fputs("ERROR: No pages were found in the print job.\n", stderr);
        return 1;
    }

    if (status == 0)
        fputs("INFO: Ready to print.\n", stderr);

    return status;
}
