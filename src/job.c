/*
 * job.c - PJL job framing and page geometry (see job.h).
 *
 * The command sequence is the one the vendor driver sends for this model.
 */
#include "job.h"

#include <string.h>

#define UEL "\033%-12345X"

void br_pjl_string(char *dst, size_t dst_size, const char *src)
{
    size_t n = 0;

    if (src) {
        for (; *src && n + 1 < dst_size; src++) {
            unsigned char ch = (unsigned char)*src;

            if (ch < 0x20 || ch == 0x7F)
                ch = ' '; /* control characters would end the PJL line */
            else if (ch == '"')
                ch = '\''; /* a quote would end the PJL string        */
            else if (ch >= 0x80)
                ch = '?'; /* the printer is told Roman-8, not UTF-8   */

            dst[n++] = (char)ch;
        }
    }
    dst[n] = '\0';
}

int br_output_dpi(br_resolution res)
{
    switch (res) {
    case BR_RES_300:
        return 300;
    case BR_RES_1200:
        return 1200;
    case BR_RES_600:
    case BR_RES_600_SMOOTH:
    default:
        return 600;
    }
}

/* Standard papers, ordered by width and then length (PostScript points). */
static const struct {
    const char *pjl;
    int         width;
    int         length;
} br_papers[] = {
    {"A5", 420, 595},
    {"B5", 499, 709},              /* ISO B5            */
    {"JISB5", 516, 729},           /* JIS B5            */
    {"SIXTEENK184X260", 522, 737}, /* 16K 184 x 260 mm  */
    {"EXECUTIVE", 522, 756},
    {"SIXTEENK195X270", 553, 765}, /* 16K 195 x 270 mm  */
    {"ROC16K", 558, 774},          /* 16K 197 x 273 mm  */
    {"A5L", 595, 420},             /* A5 long edge      */
    {"270MM", 595, 765},           /* 210 x 270 mm      */
    {"A4", 595, 842},
    {"LETTER", 612, 792},
    {"FOLIO", 612, 936},
    {"LEGAL", 612, 1008},
};

#define BR_PAPER_COUNT (sizeof(br_papers) / sizeof(br_papers[0]))

const char *br_paper_for_size(unsigned width_pt, unsigned length_pt,
                              int *extra_pt)
{
    for (size_t i = 0; i < BR_PAPER_COUNT; i++) {
        if ((int)length_pt <= br_papers[i].length &&
            (int)width_pt <= br_papers[i].width) {
            *extra_pt = br_papers[i].width - (int)width_pt;
            return br_papers[i].pjl;
        }
    }

    /* Larger than anything the printer feeds: use its biggest paper. */
    *extra_pt = 0;
    return br_papers[BR_PAPER_COUNT - 1].pjl;
}

int br_left_margin_bytes(unsigned left_margin_pt, int extra_pt, int dpi)
{
    /* The engine starts printing this many dots in from the paper edge. */
    int engine_offset;

    switch (dpi) {
    case 150:
        engine_offset = 20;
        break;
    case 300:
        engine_offset = 40;
        break;
    case 1200:
        engine_offset = 160;
        break;
    default:
        engine_offset = 80;
        break;
    }

    const long dots =
        ((long)left_margin_pt + extra_pt / 2) * dpi / 72 - engine_offset;

    if (dots + 7 < 0)
        return 0;
    return (int)((dots + 7) >> 3);
}

static const char *media_type_name(unsigned media_type)
{
    switch (media_type) {
    case 2:
        return "THIN";
    case 3:
        return "THICK";
    case 4:
        return "THICK2";
    case 5:
        return "BOND";
    case 6:
        return "TRANSPARENCY";
    case 7:
        return "ENVELOPES";
    case 8:
        return "ENVTHICK";
    case 9:
        return "ENVTHIN";
    case 10:
        return "RECYCLED";
    case 11:
        return "LABEL";
    case 12:
        return "POSTCARD";
    default:
        return "REGULAR";
    }
}

int br_job_begin(FILE *out, const br_job *job)
{
    static const char *const days[]   = {"Sun", "Mon", "Tue", "Wed",
                                         "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr",
                                         "May", "Jun", "Jul", "Aug",
                                         "Sep", "Oct", "Nov", "Dec"};
    struct tm utc, local;
    int       extra_pt;

    gmtime_r(&job->now, &utc);
    localtime_r(&job->now, &local);

    fputs(UEL "@PJL\n", out);

    /* Job identification. */
    fputs("@PJL SET HOLD=OFF\n", out);
    fprintf(out, "@PJL SET JOBNAME=\"%s\"\n", job->title);
    fprintf(out, "@PJL SET USERNAME=\"%s\"\n", job->user);
    fprintf(out, "@PJL JOB NAME=\"%s\"\n", job->title);
    fputs("@PJL PRINTLOG ITEM = 1,PRINTER\n", out);
    fprintf(out, "@PJL PRINTLOG ITEM = 2,%s,%d %s %d %d:%d:%d\n",
            days[utc.tm_wday], utc.tm_mday, months[utc.tm_mon],
            utc.tm_year + 1900, utc.tm_hour, utc.tm_min, utc.tm_sec);
    fprintf(out, "@PJL PRINTLOG ITEM = 3,%s\n", job->user);
    fputs("@PJL PRINTLOG ITEM = 4,Macintosh\n", out);
    fputs("@PJL SET STRINGCODESET=HPROMAN8\n", out);
    fprintf(out, "@PJL SET LOGINUSER=\"%s\"\n", job->user);

    /* Print quality. */
    switch (job->resolution) {
    case BR_RES_300:
        fputs("@PJL SET RESOLUTION = 300\n", out);
        break;
    case BR_RES_1200:
        fputs("@PJL SET RESOLUTION = 1200\n", out);
        fputs("@PJL SET PAPERFEEDSPEED = FULL\n", out);
        break;
    case BR_RES_600:
    case BR_RES_600_SMOOTH:
        fputs("@PJL SET RESOLUTION = 600\n", out);
        break;
    }

    fprintf(out, "@PJL SET ECONOMODE = %s\n", job->toner_save ? "ON" : "OFF");
    fprintf(out, "@PJL SET LESSPAPERCURL=%s\n",
            job->improve == BR_IMPROVE_REDUCE_CURL ? "ON" : "OFF");
    fprintf(out, "@PJL SET FIXINTENSITYUP=%s\n",
            job->improve == BR_IMPROVE_TONER_FIXING ? "ON" : "OFF");
    fputs("@PJL SET TRANSFERLEVELUP=ON\n", out);
    fputs("@PJL TRANSFERLEVEL=0\n", out);
    fprintf(out, "@PJL SET MEDIATYPE=%s\n", media_type_name(job->media_type));

    if (job->density_set) {
        fprintf(out, "@PJL SET DENSITY = %d\n", job->density);
        fprintf(out, "@PJL SET DEVBIASADJUST = %d\n", job->density);
    }

    /* The vendor driver pins this model to its one minute sleep timer. */
    fputs("@PJL DEFAULT AUTOSLEEP = ON\n", out);
    fputs("@PJL DEFAULT TIMEOUTSLEEP = 1\n", out);
    fputs("@PJL SET AUTOSLEEP = ON\n", out);
    fputs("@PJL SET TIMEOUTSLEEP = 1\n", out);

    fprintf(out, "@PJL SET JOBTIME = \"%04d%02d%02d%02d%02d%02d\"\n",
            local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
            local.tm_hour, local.tm_min, local.tm_sec);
    fputs("@PJL SET ORIENTATION = PORTRAIT\n", out);

    const char *paper =
        br_paper_for_size(job->page_width_pt, job->page_length_pt, &extra_pt);
    fprintf(out, "@PJL SET PAPER = %s\n", paper);
    fputs("@PJL SET PAGEPROTECT = AUTO\n", out);

    /* Switch to the raster language. */
    fputs("@PJL ENTER LANGUAGE = PCL\n", out);
    fputs("\033E", out);     /* reset                     */
    fputs("\033&l0S", out);  /* one-sided                 */
    if (!strcmp(paper, "270MM"))
        fputs("\033&l1043A", out); /* PCL page size for 210 x 270 mm */

    return ferror(out) ? -1 : 0;
}

int br_page_header(FILE *out, bool first_page, unsigned copies)
{
    if (first_page)
        fputs("\033&l7H", out); /* paper source: automatic */

    fprintf(out, "\033&l%uX", copies ? copies : 1);
    fputs("\033*b1030m", out); /* raster data, compression mode 1030 */

    return ferror(out) ? -1 : 0;
}

int br_page_trailer(FILE *out)
{
    fputc('\f', out);
    return ferror(out) ? -1 : 0;
}

int br_job_end(FILE *out, const br_job *job)
{
    fputs(UEL "@PJL\n", out);
    fprintf(out, "@PJL EOJ NAME=\"%s\"\n", job->title);
    fputs(UEL, out);

    return (fflush(out) || ferror(out)) ? -1 : 0;
}

int br_job_abort_padding(FILE *out)
{
    static const char zeros[256];

    for (int i = 0; i < 0x4000 / (int)sizeof(zeros); i++)
        fwrite(zeros, 1, sizeof(zeros), out);

    return ferror(out) ? -1 : 0;
}
