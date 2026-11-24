/*
 * brcomp.h - Brother "mode 1030" raster compression for the HL-1210W family.
 *
 * A page is sent as one long PCL-style combined escape sequence:
 *
 *     ESC * b 1030 m   <block> <block> ...   FF
 *
 * Each block is "<size+2>w" followed by a NUL byte, a line-count byte and
 * <size> bytes of line records.  A block holds at most 128 lines and at most
 * 0x4000 bytes of records.
 *
 * Each line record is one of:
 *     0xFF                    - a completely white line
 *     0x00                    - identical to the previous line
 *     <n> <cmd 1> ... <cmd n> - n edit commands applied to the previous line
 *
 * Edit commands (offset = bytes of the previous line to keep first):
 *     0oooolll  [offset ext] [count ext] <count bytes>   substitute (literal)
 *     1oolllll  [offset ext] [count ext] <1 byte>        repeat
 *
 * A field that is all ones is followed by extension bytes: each 0xFF adds
 * 255 and the first byte that is not 0xFF adds its value and ends the field.
 * Literal counts are stored minus 1, repeat counts minus 2.
 */
#ifndef HL1210W_BRCOMP_H
#define HL1210W_BRCOMP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BR_BLOCK_MAX_BYTES 0x4000 /* flush before a block would exceed this */
#define BR_BLOCK_MAX_LINES 128    /* lines per block (count is one byte)    */
#define BR_LINE_MAX_CMDS   0xFD   /* commands before the rest goes literal  */

/*
 * Encode one line against a reference line.
 *
 *   cur, ref  - current and reference rows, 'n' bytes each
 *   margin    - number of white bytes the printer inserts ahead of the row
 *   out       - receives the record; needs br_line_bound(n) bytes
 *
 * Returns the record length, or 0 when the row equals the reference (the
 * caller then emits the single byte 0x00).
 */
size_t br_encode_line(const uint8_t *cur, const uint8_t *ref, int n,
                      int margin, uint8_t *out);

/* Worst-case size of one encoded line of n bytes. */
size_t br_line_bound(int n);

typedef struct br_page {
    FILE    *out;
    int      n;          /* bytes per row that carry pixels            */
    int      margin;     /* leading white bytes                        */
    uint8_t *ref;        /* previous row                               */
    uint8_t *line;       /* scratch for one encoded line               */
    uint8_t *block;      /* pending block payload                      */
    size_t   block_len;
    unsigned block_lines;
    unsigned page_lines; /* lines emitted since the start of the page  */
    int      error;
} br_page;

/* Start a page.  Returns 0 on success, -1 if out of memory. */
int br_page_begin(br_page *pg, FILE *out, int bytes_per_row, int margin);

/* Add the next row (bytes_per_row bytes, MSB first, 1 = black). */
int br_page_add_row(br_page *pg, const uint8_t *row);

/* Flush the pending block and release memory.  Returns 0 or -1. */
int br_page_end(br_page *pg);

#endif /* HL1210W_BRCOMP_H */
