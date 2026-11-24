#!/usr/bin/env python3
"""Round-trip fuzz test: random pages through the driver's encoder, back
through the independent decoder, compared with the halftoned input.

Usage: fuzz_roundtrip.py BUILD_DIR [COUNT] [SEED]

The vendor comparison (tests/golden) pins the encoder to Brother's output for
a fixed set of pages.  This test goes wide instead: odd widths, heights around
the 128-line block boundary, margins around the points where the command
fields overflow into extension bytes, sparse and dense content.
"""
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import brdecode  # noqa: E402


def halftone(pixels, width, height, matrix):
    nbytes = (width + 7) // 8
    rows = []
    for y in range(height):
        mrow = matrix[(y % 32) * 32:(y % 32) * 32 + 32]
        out = bytearray(nbytes)
        for x in range(width):
            px = pixels[y * width + x]
            if px > mrow[x & 31] or px == 255:
                out[x >> 3] |= 0x80 >> (x & 7)
        rows.append(bytes(out))
    return rows


def random_page(rng, width, height):
    kind = rng.randrange(7)
    step = rng.choice((1, 3, 8, 16, 40))
    px = bytearray(width * height)
    for y in range(height):
        row_kind = rng.randrange(10)
        if row_kind == 0:
            continue                                        # blank row
        if row_kind == 1 and y:
            px[y * width:(y + 1) * width] = px[(y - 1) * width:y * width]
            continue                                        # repeated row
        for x in range(width):
            if kind == 0:
                v = rng.randrange(256)
            elif kind == 1:
                v = rng.choice((0, 255))
            elif kind == 2:
                v = 255 if rng.random() < 0.03 else 0
            elif kind == 3:
                v = 255 if (x // step) % 2 else 0
            elif kind == 4:
                v = 255 if (x + y) % 23 < 9 else (3 if x % 5 == 0 else 0)
            elif kind == 5:
                v = (x * 7 + y * 3) % 256
            else:  # diagonals with near-white and near-black neighbours
                v = (255 if abs(x - y) < 4 or abs(width - x - y) < 3
                     else rng.choice((0, 0, 0, 2, 14, 15, 254)))
            px[y * width + x] = v
    return bytes(px)


def main():
    build = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build")
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 250
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1210
    tool = os.path.join(build, "hl1210w-testtool")
    work = os.path.join(build, "fuzz")
    os.makedirs(work, exist_ok=True)
    rng = random.Random(seed)

    src, mat, out, wide_path = (os.path.join(work, name) for name in
                                ("page.gray", "matrix.bin", "page.out", "page.x2"))
    failures = 0
    for case in range(count):
        if case % 4 == 0:
            width = rng.choice((1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 33, 64, 65))
        else:
            width = rng.randrange(1, 700)
        if case % 5 == 0:
            height = rng.choice((1, 2, 3, 127, 128, 129, 130, 257))
        else:
            height = rng.randrange(1, 200)
        if width * height > 60000:
            height = max(1, 60000 // width)
        margin = rng.choice((0, 0, 1, 2, 3, 5, 33, 34, 35, 289, 400))
        mode = "smooth" if case % 3 == 0 else "native"
        matrix = bytes(rng.randrange(256) for _ in range(1024))
        pixels = random_page(rng, width, height)

        with open(src, "wb") as f:
            f.write(pixels)
        with open(mat, "wb") as f:
            f.write(matrix)
        subprocess.run([tool, "page", mode, str(width), str(height),
                        str(margin), mat, src, out], check=True)

        if mode == "smooth":
            subprocess.run([tool, "smooth", str(width), str(height), src,
                            wide_path], check=True)
            ew = (2 * width + 3) & ~3
            with open(wide_path, "rb") as f:
                wide = f.read()
            doubled = b"".join(wide[y * ew:(y + 1) * ew] * 2 for y in range(height))
            want = halftone(doubled, ew, 2 * height, matrix)
        else:
            want = halftone(pixels, width, height, matrix)

        with open(out, "rb") as f:
            stream = f.read()
        try:
            rows = brdecode.decode_blocks(stream)
            nbytes = len(want[0])
            ok = len(rows) == len(want) and all(
                got.ljust(margin + nbytes, b"\0") == b"\0" * margin + expect
                for got, expect in zip(rows, want))
        except brdecode.FormatError as err:
            ok = False
            print("case %d: %s" % (case, err))
        if not ok:
            failures += 1
            print("FAIL  case %d: %s, %dx%d, margin %d (seed %d)"
                  % (case, mode, width, height, margin, seed))

    print("%d random pages, %d failures" % (count, failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
