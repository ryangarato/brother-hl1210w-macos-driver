#!/usr/bin/env python3
"""Decode an HL-1210W print stream (PJL + "mode 1030" raster) back to bitmaps.

Written independently of the encoder, straight from the data format, so it
doubles as a check that what the driver sends is well formed.

Usage: brdecode.py FILE.prn [--png PREFIX] [--quiet]

As a module:
    job = parse_job(data)        -> Job(pjl=[...], pcl=[...], pages=[Page])
    rows = decode_blocks(data)   -> list of bytes objects, one per raster row
"""
import struct
import sys
import zlib

UEL = b"\x1b%-12345X"


class FormatError(Exception):
    pass


class Page:
    def __init__(self):
        self.commands = []  # PCL commands seen ahead of the raster data
        self.rows = []      # decoded rows (bytes), MSB first, 1 = black
        self.blocks = []    # (payload bytes, line count) per block

    @property
    def width_bytes(self):
        return max((len(r) for r in self.rows), default=0)


class Job:
    def __init__(self):
        self.pjl = []
        self.trailer_pjl = []
        self.pcl = []
        self.pages = []


def _extension(data, pos):
    """Read a field extension: 0xFF bytes add 255 each, the next byte ends it."""
    total = 0
    while True:
        if pos >= len(data):
            raise FormatError("extension runs past the end of the block")
        b = data[pos]
        pos += 1
        total += b
        if b != 0xFF:
            return total, pos


def decode_records(payload, lines, row, out_rows):
    """Decode 'lines' line records from one block payload.

    'row' is the printer's row buffer (a bytearray) carried from line to line.
    """
    pos = 0
    for _ in range(lines):
        if pos >= len(payload):
            raise FormatError("block ends before its last line")
        count = payload[pos]
        pos += 1

        if count == 0xFF:              # white line
            for i in range(len(row)):
                row[i] = 0
        elif count != 0:               # edit commands against the previous line
            at = 0
            for _cmd in range(count):
                if pos >= len(payload):
                    raise FormatError("line ends in the middle of its commands")
                head = payload[pos]
                pos += 1
                if head & 0x80:        # repeat one byte
                    offset = (head >> 5) & 0x03
                    if offset == 3:
                        extra, pos = _extension(payload, pos)
                        offset += extra
                    n = head & 0x1F
                    if n == 31:
                        extra, pos = _extension(payload, pos)
                        n += extra
                    n += 2
                    if pos >= len(payload):
                        raise FormatError("repeat command without its byte")
                    chunk = bytes([payload[pos]]) * n
                    pos += 1
                else:                  # substitute bytes
                    offset = (head >> 3) & 0x0F
                    if offset == 15:
                        extra, pos = _extension(payload, pos)
                        offset += extra
                    n = head & 0x07
                    if n == 7:
                        extra, pos = _extension(payload, pos)
                        n += extra
                    n += 1
                    chunk = payload[pos:pos + n]
                    if len(chunk) != n:
                        raise FormatError("substitute command runs past the block")
                    pos += n
                at += offset
                end = at + len(chunk)
                if end > len(row):
                    row.extend(b"\0" * (end - len(row)))
                row[at:end] = chunk
                at = end
        # count == 0: same as the previous line
        out_rows.append(bytes(row))

    if pos != len(payload):
        raise FormatError("%d stray bytes after the last line of a block"
                          % (len(payload) - pos))


def read_blocks(data, pos, page):
    """Read "<n>w" blocks starting at data[pos]; returns the position after."""
    row = bytearray()
    while pos < len(data) and data[pos:pos + 1].isdigit():
        end = pos
        while end < len(data) and data[end:end + 1].isdigit():
            end += 1
        if data[end:end + 1] != b"w":
            raise FormatError("block length at %d is not followed by 'w'" % pos)
        size = int(data[pos:end])
        body = data[end + 1:end + 1 + size]
        if len(body) != size or size < 2:
            raise FormatError("truncated block at %d" % pos)
        if body[0] != 0:
            raise FormatError("block at %d does not start with a NUL" % pos)
        lines = body[1]
        payload = body[2:]
        if lines == 0 or lines > 128:
            raise FormatError("block at %d has %d lines" % (pos, lines))
        if len(payload) > 0x4000:
            raise FormatError("block at %d is %d bytes" % (pos, len(payload)))
        decode_records(payload, lines, row, page.rows)
        page.blocks.append((payload, lines))
        pos = end + 1 + size
    return pos


def decode_blocks(data):
    """Decode a bare sequence of blocks (no job framing)."""
    page = Page()
    pos = read_blocks(data, 0, page)
    if pos != len(data):
        raise FormatError("garbage after the last block at %d" % pos)
    return page.rows


def _pcl_command(data, pos):
    """Parse one 'ESC X' or parameterised 'ESC & l # X' command."""
    if data[pos + 1:pos + 2] == b"E":
        return "E", pos + 2
    intro = data[pos + 1:pos + 3]
    end = pos + 3
    while end < len(data) and (data[end:end + 1].isdigit() or data[end:end + 1] in b"+-."):
        end += 1
    return (intro + data[pos + 3:end + 1]).decode("latin-1"), end + 1


def parse_job(data):
    job = Job()
    pos = 0

    if not data.startswith(UEL + b"@PJL\n"):
        raise FormatError("stream does not start with UEL @PJL")
    pos = len(UEL) + 5
    job.pjl.append("@PJL")

    while True:
        end = data.index(b"\n", pos)
        line = data[pos:end].decode("latin-1")
        pos = end + 1
        job.pjl.append(line)
        if not line.startswith("@PJL"):
            raise FormatError("not a PJL line: %r" % line)
        if line.startswith("@PJL ENTER LANGUAGE"):
            break

    page = None
    while pos < len(data):
        if data.startswith(UEL, pos):
            break
        ch = data[pos:pos + 1]
        if ch == b"\x1b":
            cmd, nxt = _pcl_command(data, pos)
            if cmd == "*b1030m":
                if page is None:
                    page = Page()
                pos = read_blocks(data, nxt, page)
                if data[pos:pos + 1] != b"\f":
                    raise FormatError("page raster is not ended by a form feed")
                pos += 1
                job.pages.append(page)
                page = None
            else:
                if cmd in ("E",) or cmd.endswith("S") or cmd.endswith("A"):
                    job.pcl.append(cmd)
                else:
                    if page is None:
                        page = Page()
                    page.commands.append(cmd)
                pos = nxt
        else:
            raise FormatError("unexpected byte 0x%02x at %d" % (data[pos], pos))

    if page is not None:
        raise FormatError("page commands without raster data at the end")

    tail = data[pos:]
    if not tail.startswith(UEL + b"@PJL\n") or not tail.endswith(UEL):
        raise FormatError("job does not end with UEL @PJL ... UEL")
    for line in tail[len(UEL):-len(UEL)].split(b"\n"):
        if line:
            job.trailer_pjl.append(line.decode("latin-1"))
    return job


def write_png(path, rows, width_bytes):
    """Write decoded rows as a 1-bit PNG (black on white)."""
    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    raw = bytearray()
    for row in rows:
        padded = row + b"\0" * (width_bytes - len(row))
        raw.append(0)
        raw.extend(b ^ 0xFF for b in padded)
    header = struct.pack(">IIBBBBB", width_bytes * 8, len(rows), 1, 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    with open(argv[1], "rb") as f:
        data = f.read()
    job = parse_job(data)
    quiet = "--quiet" in argv
    prefix = argv[argv.index("--png") + 1] if "--png" in argv else None

    if not quiet:
        for line in job.pjl + ["..."] + job.trailer_pjl:
            print(line)
        print("PCL setup:", " ".join("ESC" + c for c in job.pcl))
    for number, page in enumerate(job.pages, 1):
        black = sum(bin(b).count("1") for row in page.rows for b in row)
        total = max(1, len(page.rows) * page.width_bytes * 8)
        print("page %d: %s; %d rows x %d bytes, %d blocks, %.2f%% black"
              % (number, " ".join("ESC" + c for c in page.commands),
                 len(page.rows), page.width_bytes, len(page.blocks),
                 100.0 * black / total))
        if prefix:
            write_png("%s-%d.png" % (prefix, number), page.rows, page.width_bytes)


if __name__ == "__main__":
    main(sys.argv)
