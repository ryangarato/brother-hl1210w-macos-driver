/*
 * job.h - PJL job framing and page geometry for the Brother HL-1210W.
 */
#ifndef HL1210W_JOB_H
#define HL1210W_JOB_H

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

typedef enum {
    BR_RES_300 = 0,    /* 300 dpi rendering, 300 dpi output              */
    BR_RES_600,        /* 600 dpi rendering, 600 dpi output              */
    BR_RES_600_SMOOTH, /* 300 dpi rendering, smoothed up to 600 dpi      */
    BR_RES_1200        /* 1200 dpi rendering ("HQ 1200")                 */
} br_resolution;

typedef enum {
    BR_IMPROVE_OFF = 0,
    BR_IMPROVE_REDUCE_CURL,
    BR_IMPROVE_TONER_FIXING
} br_improve;

typedef struct br_job {
    char          user[81];  /* sanitised for PJL, at most 80 characters */
    char          title[81];
    br_resolution resolution;
    bool          text_screen; /* "Text" halftone instead of "Graphics"  */
    bool          toner_save;
    br_improve    improve;
    bool          skip_blank;
    bool          density_set;
    int           density;    /* -6 .. 6, used when density_set          */
    unsigned      media_type; /* cupsMediaType                           */
    unsigned      page_width_pt;  /* PageSize[0]                         */
    unsigned      page_length_pt; /* PageSize[1]                         */
    unsigned      left_margin_pt; /* Margins[0]                          */
    time_t        now;
} br_job;

/* Copy a job string, keeping only what is safe inside a PJL "..." value. */
void br_pjl_string(char *dst, size_t dst_size, const char *src);

/* Dots per inch the printer is driven at for a resolution setting. */
int br_output_dpi(br_resolution res);

/*
 * Pick the PJL paper name for a page.  The printer is told the smallest
 * standard paper that contains the page; *extra_pt receives how much wider
 * (in points) that paper is, so the image can be centred on it.
 */
const char *br_paper_for_size(unsigned width_pt, unsigned length_pt,
                              int *extra_pt);

/*
 * Number of white bytes to send ahead of every raster row so that the image
 * lands at the left edge of the imageable area.
 */
int br_left_margin_bytes(unsigned left_margin_pt, int extra_pt, int dpi);

/* Each returns 0 on success and -1 on a write error. */
int br_job_begin(FILE *out, const br_job *job);
int br_page_header(FILE *out, bool first_page, unsigned copies);
int br_page_trailer(FILE *out);
int br_job_end(FILE *out, const br_job *job);

/* Sent when a job is cancelled mid-page so the printer drops partial data. */
int br_job_abort_padding(FILE *out);

#endif /* HL1210W_JOB_H */
