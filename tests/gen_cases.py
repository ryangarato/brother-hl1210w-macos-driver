#!/usr/bin/env python3
"""Create the synthetic test pages used to compare the native driver with
Brother's original routines.

Usage: gen_cases.py OUT_DIR

Writes 8-bit black (K) images (0 = white, 255 = black), raw 32x32 halftone
matrices, and "cases.tsv" - one test per line - into OUT_DIR.  Everything is
synthetic and reproducible; no vendor data is needed.
"""
import math
import os
import random
import sys


class Image:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h)

    def set(self, x, y, v):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = max(0, min(255, int(v)))

    def rect(self, x0, y0, x1, y1, v=255):
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                self.px[y * self.w + x] = v

    def disc(self, cx, cy, r, v=255, soft=False):
        for y in range(int(cy - r - 2), int(cy + r + 3)):
            for x in range(int(cx - r - 2), int(cx + r + 3)):
                d = math.hypot(x - cx, y - cy)
                if d <= r:
                    self.set(x, y, v)
                elif soft and d <= r + 1:
                    self.set(x, y, v * (r + 1 - d))

    def line(self, x0, y0, x1, y1, width=1, v=255):
        steps = int(max(abs(x1 - x0), abs(y1 - y0))) * 2 + 1
        for i in range(steps + 1):
            t = i / steps
            x = x0 + (x1 - x0) * t
            y = y0 + (y1 - y0) * t
            for dy in range(width):
                for dx in range(width):
                    self.set(int(x) + dx, int(y) + dy, v)

    def triangle(self, x0, y0, size, flip=False, v=255):
        for i in range(size):
            row = y0 + i
            span = i if not flip else size - i
            self.rect(x0, row, x0 + span, row + 1, v)

    def save(self, path):
        with open(path, "wb") as f:
            f.write(self.px)


def glyphs(img, rng, x0, y0, x1, y1, cell=9):
    """Blocky pseudo-text: lots of corners, stems and diagonals."""
    y = y0
    while y + cell < y1:
        x = x0
        while x + cell < x1:
            kind = rng.randrange(6)
            if kind == 0:
                img.rect(x, y, x + 2, y + cell - 2)
                img.rect(x, y, x + cell - 3, y + 2)
            elif kind == 1:
                img.line(x, y, x + cell - 3, y + cell - 3, 2)
            elif kind == 2:
                img.line(x + cell - 3, y, x, y + cell - 3, 2)
            elif kind == 3:
                img.disc(x + cell // 2 - 1, y + cell // 2 - 1, cell // 2 - 1)
            elif kind == 4:
                img.rect(x, y + cell // 2 - 1, x + cell - 2, y + cell // 2 + 1)
                img.rect(x + cell // 2 - 1, y, x + cell // 2 + 1, y + cell - 2)
            x += cell
        y += cell + 2


def make_images(out_dir):
    rng = random.Random(1210)
    images = {}

    # 1. Shapes with every kind of diagonal edge (exercises the smoothing).
    im = Image(200, 150)
    im.disc(40, 40, 28)
    im.disc(110, 40, 22, soft=True)
    im.triangle(150, 10, 40)
    im.triangle(150, 60, 40, flip=True)
    for i in range(6):
        im.line(5 + i * 12, 80, 60 + i * 12, 145, 1 + i % 3)
        im.line(190 - i * 9, 82, 120 - i * 14, 148, 1 + i % 2)
    im.rect(0, 0, 200, 1)
    im.rect(0, 149, 200, 150)
    im.rect(0, 0, 1, 150)
    im.rect(199, 0, 200, 150)
    images["shapes"] = im

    # 2. Pseudo text.
    im = Image(320, 180)
    glyphs(im, rng, 6, 6, 314, 174)
    images["glyphs"] = im

    # 3. Gradients and mid-tones (halftone thresholds).
    im = Image(256, 96)
    for y in range(96):
        for x in range(256):
            im.px[y * 256 + x] = x if y < 48 else (x * 3 + y * 5) % 256
    images["gradient"] = im

    # 4. Blank rows, repeated rows, long runs, far-apart marks.
    im = Image(1300, 300)
    im.rect(0, 10, 1300, 30)          # solid black band
    im.rect(100, 40, 110, 120)        # vertical bar: identical rows
    im.rect(1280, 60, 1290, 100)      # mark far to the right (long offsets)
    for y in range(130, 200):
        for x in range(0, 1300, 16):
            im.rect(x, y, x + 8, y + 1)   # 0xFF00 pattern rows
    for y in range(210, 290):
        im.rect(y * 3, y, y * 3 + 300, y + 1)  # sliding long run
    images["runs"] = im

    # 5. Tall and narrow: crosses the 128-line block boundary several times.
    im = Image(70, 420)
    for y in range(420):
        for x in range(70):
            if (x + y // 3) % 11 < 4 and y % 37 > 4:
                im.px[y * 70 + x] = 255
            elif (x * y) % 97 == 0:
                im.px[y * 70 + x] = 128
    images["tall"] = im

    # 6. Wide noise: more than 253 commands per line, 16 KB block flushes.
    im = Image(4900, 150)
    for y in range(150):
        base = y * 4900
        if y % 10 == 9:
            continue                                  # blank row
        if y % 10 == 8:
            im.px[base:base + 4900] = im.px[base - 4900:base]  # same as above
            continue
        for x in range(4900):
            mode = (y // 30) % 5
            if mode == 0:
                v = rng.randrange(256)
            elif mode == 1:
                v = 255 if (x // 8) % 2 == 0 else 0     # alternate bytes
            elif mode == 2:
                v = rng.choice((0, 0, 0, 255))
            elif mode == 3:
                v = 255 if rng.random() < 0.02 else 0
            else:
                v = 100 + rng.randrange(60)
            im.px[base + x] = v
    images["noise"] = im

    # 7. Page-like mix at 300 dpi width for the smoothing mode.
    im = Image(1220, 240)
    glyphs(im, rng, 40, 10, 700, 120, cell=13)
    im.disc(900, 70, 55)
    im.disc(1080, 70, 40, soft=True)
    for i in range(10):
        im.line(40 + i * 60, 130, 300 + i * 80, 235, 1 + i % 4)
    for y in range(140, 230):
        for x in range(820, 1200):
            im.px[y * 1220 + x] = (x - 820) * 255 // 380
    images["page300"] = im

    for name, image in images.items():
        image.save(os.path.join(out_dir, name + ".gray"))
    return images


def make_matrices(out_dir):
    """Three threshold matrices with different character."""
    rng = random.Random(32)
    matrices = {}

    # Clustered dots, like a real laser screen.
    cells = []
    for y in range(32):
        for x in range(32):
            spot = (math.cos((x + y) * math.pi / 4) +
                    math.cos((x - y) * math.pi / 4))
            cells.append((-(spot + ((x * 7 + y * 13) % 31) / 1e4), y * 32 + x))
    cells.sort()
    dots = bytearray(1024)
    for rank, (_, index) in enumerate(cells):
        dots[index] = rank * 255 // 1024
    matrices["dots"] = bytes(dots)

    # Uniform noise, including the extreme thresholds 0 and 255.
    noise = bytearray(rng.randrange(256) for _ in range(1024))
    noise[0], noise[33], noise[700] = 0, 255, 255
    matrices["noise"] = bytes(noise)

    # A different ramp on every row.
    matrices["ramp"] = bytes((x * 8 + y * 37) % 256
                             for y in range(32) for x in range(32))

    paths = {}
    for name, table in matrices.items():
        paths[name] = os.path.join(out_dir, name + ".matrix")
        with open(paths[name], "wb") as f:
            f.write(table)
    return paths


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)

    matrices = make_matrices(out_dir)
    images = make_images(out_dir)

    # (name, kind, image, mode, hwres, margin, matrix)
    plan = [
        ("shapes-600", "page", "shapes", "native", 600, 3, "dots"),
        ("shapes-smooth", "page", "shapes", "smooth", 300, 3, "dots"),
        ("glyphs-smooth", "page", "glyphs", "smooth", 300, 3, "noise"),
        ("glyphs-300", "page", "glyphs", "native", 300, 2, "ramp"),
        ("gradient-600", "page", "gradient", "native", 600, 3, "dots"),
        ("gradient-1200", "page", "gradient", "native", 1200, 5, "noise"),
        ("gradient-ramp", "page", "gradient", "native", 600, 2, "ramp"),
        # A one-byte margin cannot be written as a repeat; the vendor code
        # mis-encodes it (it never occurs with real margins), so cases named
        # "own-..." are checked by decoding only.
        ("own-margin1", "page", "gradient", "native", 600, 1, "ramp"),
        ("gradient-smooth", "page", "gradient", "smooth", 300, 0, "dots"),
        ("runs-600", "page", "runs", "native", 600, 3, "dots"),
        ("runs-margin", "page", "runs", "native", 600, 300, "noise"),
        ("tall-600", "page", "tall", "native", 600, 40, "dots"),
        ("tall-smooth", "page", "tall", "smooth", 300, 3, "ramp"),
        ("noise-600", "page", "noise", "native", 600, 3, "dots"),
        ("noise-noise", "page", "noise", "native", 1200, 5, "noise"),
        ("page300-smooth", "page", "page300", "smooth", 300, 3, "dots"),
        ("shapes-x2", "smooth", "shapes", "", 0, 0, ""),
        ("glyphs-x2", "smooth", "glyphs", "", 0, 0, ""),
        ("gradient-x2", "smooth", "gradient", "", 0, 0, ""),
        ("page300-x2", "smooth", "page300", "", 0, 0, ""),
    ]

    with open(os.path.join(out_dir, "cases.tsv"), "w") as f:
        for name, kind, image, mode, hwres, margin, matrix in plan:
            im = images[image]
            src = os.path.join(out_dir, image + ".gray")
            if kind == "page":
                f.write("\t".join(["page", name, mode, str(im.w), str(im.h),
                                   str(hwres), str(margin), matrices[matrix],
                                   src]) + "\n")
            else:
                f.write("\t".join(["smooth", name, str(im.w), str(im.h),
                                   src]) + "\n")
        for left, extra, dpi in ((12, 0, 300), (12, 0, 600), (12, 0, 1200),
                                 (12, 16, 600), (12, 17, 600), (18, 96, 300),
                                 (0, 0, 600), (5, 0, 600), (12, 192, 1200),
                                 (9, 0, 600), (10, 0, 600), (36, 30, 1200)):
            f.write("\t".join(["margin", "margin-%d-%d-%d" % (left, extra, dpi),
                               str(left), str(extra), str(dpi)]) + "\n")

        # Job framing: every paper at every resolution, then option mixes.
        # Fields: vendor resolution choice, text screen, toner save, improve
        # output, density (99 = printer default), media type, page size.
        epoch = "1795482123"  # 2026-11-24 01:02:03 UTC
        sizes = [(595, 842), (612, 792), (612, 1008), (522, 756), (420, 595),
                 (595, 420), (499, 709), (516, 729), (612, 936), (595, 765),
                 (553, 765), (522, 737), (558, 774),
                 (500, 700), (420, 600), (612, 1000), (430, 420), (600, 900)]
        jobs = []
        for vres in ("300dpi", "600dpi", "300x600dpi", "2400x600dpi"):
            for width, length in sizes:
                jobs.append((vres, 0, 0, 0, 99, 1, width, length))
        jobs += [
            ("600dpi", 1, 1, 0, 99, 1, 612, 792),
            ("600dpi", 0, 0, 1, 99, 10, 595, 842),
            ("300x600dpi", 0, 1, 2, -6, 10, 612, 792),
            ("300dpi", 1, 0, 2, 6, 1, 595, 842),
            ("2400x600dpi", 0, 1, 1, 0, 1, 612, 1008),
            ("600dpi", 0, 0, 0, -1, 1, 612, 792),
            ("600dpi", 0, 0, 0, 3, 2, 612, 792),
        ]
        for n, (vres, text, toner, improve, density, media, width, length) in enumerate(jobs):
            f.write("\t".join(["job", "job-%02d-%s-%dx%d" % (n, vres, width, length),
                               vres, str(text), str(toner), str(improve),
                               str(density), str(media), str(width), str(length),
                               epoch, "alex", "Quarterly report.pdf"]) + "\n")


if __name__ == "__main__":
    main()
