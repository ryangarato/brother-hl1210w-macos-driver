/*
 * brcomp.c - Brother "mode 1030" raster compression (see brcomp.h).
 *
 * The command selection below follows the vendor driver decision for decision
 * so that the byte stream matches what the printer firmware was tested with.
 */
#include "brcomp.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Append the extension bytes for a field value that did not fit. */
static uint8_t *put_ext(uint8_t *p, int rest)
{
    while (rest >= 255) {
        *p++ = 0xFF;
        rest -= 255;
    }
    *p++ = (uint8_t)rest;
    return p;
}

/* Substitute 'count' (>= 1) bytes after keeping 'offset' bytes. */
static uint8_t *put_literal(uint8_t *p, int offset, int count,
                            const uint8_t *data)
{
    uint8_t *hdr = p++;

    if (offset < 15) {
        *hdr = (uint8_t)(offset << 3);
    } else {
        *hdr = 0x78;
        p = put_ext(p, offset - 15);
    }

    if (count - 1 < 7) {
        *hdr |= (uint8_t)(count - 1);
    } else {
        *hdr |= 0x07;
        p = put_ext(p, count - 8);
    }

    memcpy(p, data, (size_t)count);
    return p + count;
}

/* Write 'count' (>= 2) copies of 'value' after keeping 'offset' bytes. */
static uint8_t *put_repeat(uint8_t *p, int offset, int count, uint8_t value)
{
    uint8_t *hdr = p++;

    if (offset < 3) {
        *hdr = (uint8_t)(0x80 | (offset << 5));
    } else {
        *hdr = 0xE0;
        p = put_ext(p, offset - 3);
    }

    if (count - 2 < 31) {
        *hdr |= (uint8_t)(count - 2);
    } else {
        *hdr |= 0x1F;
        p = put_ext(p, count - 33);
    }

    *p++ = value;
    return p;
}

size_t br_line_bound(int n)
{
    /*
     * At most 254 commands per line, each covering at least one byte and
     * costing a header plus up to two extension bytes on top of its payload;
     * extension chains add one more byte per 255 bytes skipped or copied.
     * 4n + 1024 is comfortably above that for every n.
     */
    return (size_t)n * 4 + 1024;
}

size_t br_encode_line(const uint8_t *cur, const uint8_t *ref, int n,
                      int margin, uint8_t *out)
{
    uint8_t *p    = out + 1;
    unsigned cmds = 0;
    int      pos  = 0;
    bool     first = true;

    if (margin >= 2) {
        p = put_repeat(p, 0, margin, 0x00);
        cmds++;
    } else if (margin == 1) {
        /* A repeat cannot express a single byte; use a literal instead. */
        static const uint8_t zero = 0;
        p = put_literal(p, 0, 1, &zero);
        cmds++;
    }

    while (pos < n) {
        if (cmds > 0xFC) {
            /* Out of command slots: send the rest of the row verbatim. */
            p = put_literal(p, 0, n - pos, cur + pos);
            cmds++;
            break;
        }

        /* Skip the bytes that already match the reference row. */
        int offset = 0;
        while (cur[pos + offset] == ref[pos + offset]) {
            offset++;
            if (pos + offset >= n) {
                if (first)
                    return 0; /* whole row unchanged */
                goto done;
            }
        }

        const int start = pos + offset;

        /* Measure the run of bytes that differ from the reference. */
        int diff = 1;
        while (start + diff < n && cur[start + diff] != ref[start + diff])
            diff++;

        if (diff < 3) {
            p = put_literal(p, offset, diff, cur + start);
            cmds++;
            pos = start + diff;
        } else {
            const int end = start + diff;
            int       at  = start;

            /* Split the differing run into repeats and literals. */
            do {
                if (at >= end)
                    break;

                if (at + 2 < end) {
                    if (cur[at] == cur[at + 1] && cur[at] == cur[at + 2]) {
                        int run = 3;
                        while (at + run < end && cur[at + run] == cur[at])
                            run++;
                        p = put_repeat(p, offset, run, cur[at]);
                        at += run;
                    } else {
                        /* Literal up to the next run of three, or the end. */
                        int len = end - at;
                        for (int j = 1; at + j + 2 < end; j++) {
                            if (cur[at + j] == cur[at + j + 1] &&
                                cur[at + j] == cur[at + j + 2]) {
                                len = j;
                                break;
                            }
                        }
                        p = put_literal(p, offset, len, cur + at);
                        at += len;
                    }
                } else {
                    p = put_literal(p, offset, end - at, cur + at);
                    at = end;
                }

                cmds++;
                offset = 0;
            } while (cmds != BR_LINE_MAX_CMDS);

            pos = at;
        }

        first = false;
    }

done:
    out[0] = (uint8_t)cmds;
    return (size_t)(p - out);
}

/* --------------------------------------------------------------------- */

static void flush_block(br_page *pg)
{
    if (pg->block_len > 0) {
        if (fprintf(pg->out, "%zuw%c%c", pg->block_len + 2, 0,
                    (int)(pg->block_lines & 0xFF)) < 0 ||
            fwrite(pg->block, 1, pg->block_len, pg->out) != pg->block_len)
            pg->error = 1;
    }
    pg->block_len   = 0;
    pg->block_lines = 0;
}

static void save_record(br_page *pg, const uint8_t *rec, size_t len)
{
    if (pg->block_len + len > BR_BLOCK_MAX_BYTES ||
        (pg->page_lines % BR_BLOCK_MAX_LINES) == 0)
        flush_block(pg);

    memcpy(pg->block + pg->block_len, rec, len);
    pg->block_len += len;
    pg->block_lines++;
    pg->page_lines++;
}

int br_page_begin(br_page *pg, FILE *out, int bytes_per_row, int margin)
{
    memset(pg, 0, sizeof(*pg));
    pg->out    = out;
    pg->n      = bytes_per_row;
    pg->margin = margin > 0 ? margin : 0;

    const size_t bound = br_line_bound(bytes_per_row) + (size_t)pg->margin / 255;

    pg->ref   = calloc(1, (size_t)bytes_per_row);
    pg->line  = malloc(bound);
    pg->block = malloc(BR_BLOCK_MAX_BYTES + bound);

    if (!pg->ref || !pg->line || !pg->block) {
        free(pg->ref);
        free(pg->line);
        free(pg->block);
        memset(pg, 0, sizeof(*pg));
        return -1;
    }
    return 0;
}

static bool row_is_blank(const uint8_t *row, int n)
{
    for (int i = 0; i < n; i++)
        if (row[i])
            return false;
    return true;
}

int br_page_add_row(br_page *pg, const uint8_t *row)
{
    static const uint8_t blank_line = 0xFF;
    static const uint8_t same_line  = 0x00;

    if (row_is_blank(row, pg->n)) {
        save_record(pg, &blank_line, 1);
    } else {
        /*
         * The first line of the page and the first line of every 128-line
         * group are coded against their own inverse, i.e. in full, so that a
         * group never depends on rows that came before it.
         */
        if ((pg->page_lines % BR_BLOCK_MAX_LINES) == 0) {
            for (int i = 0; i < pg->n; i++)
                pg->ref[i] = (uint8_t)~row[i];
        }

        size_t len = br_encode_line(row, pg->ref, pg->n, pg->margin, pg->line);
        if (len == 0)
            save_record(pg, &same_line, 1);
        else
            save_record(pg, pg->line, len);
    }

    memcpy(pg->ref, row, (size_t)pg->n);
    return pg->error ? -1 : 0;
}

int br_page_end(br_page *pg)
{
    flush_block(pg);

    int rc = pg->error ? -1 : 0;
    free(pg->ref);
    free(pg->line);
    free(pg->block);
    pg->ref = pg->line = pg->block = NULL;
    return rc;
}
