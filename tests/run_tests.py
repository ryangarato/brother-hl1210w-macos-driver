#!/usr/bin/env python3
"""Self-tests for the Brother HL-1210W native driver.

Usage: run_tests.py BUILD_DIR

  golden    the driver's output for synthetic pages equals what Brother's own
            routines produce for them (tests/golden, captured with
            tests/compare_original.py)
  decode    the compressed raster decodes, with an independent decoder, to the
            halftoned bitmap computed here in Python
  framing   the PJL / PCL job framing is the expected command sequence
  filter    the real filter binary, fed CUPS raster files, end to end
  paper     paper names and centring for every size in the PPD
  ppd       cupstestppd accepts the PPD
"""
import gzip
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import brdecode  # noqa: E402
from caselib import load_cases, native_output  # noqa: E402

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build")
TOOL = os.path.join(BUILD, "hl1210w-testtool")
FILTER = os.path.join(BUILD, "rastertohl1210w")
WORK = os.path.join(BUILD, "tests")
GOLDEN = os.path.join(HERE, "golden")

results = []


def check(group, name, ok, detail=""):
    results.append((group, name, ok, detail))
    if not ok:
        print("FAIL  %s/%s  %s" % (group, name, detail))


def run(args, **kw):
    return subprocess.run(args, check=True, capture_output=True, **kw)


# ---- reference implementations -------------------------------------------

def halftone(pixels, width, height, matrix, first_row=0):
    """Rows of ceil(width/8) bytes; a pixel prints when darker than the
    threshold, always when fully black."""
    nbytes = (width + 7) // 8
    rows = []
    for y in range(height):
        mrow = matrix[((first_row + y) % 32) * 32:((first_row + y) % 32) * 32 + 32]
        src = pixels[y * width:(y + 1) * width]
        out = bytearray(nbytes)
        for x, px in enumerate(src):
            if px > mrow[x & 31] or px == 255:
                out[x >> 3] |= 0x80 >> (x & 7)
        rows.append(bytes(out))
    return rows


def read(path):
    with open(path, "rb") as f:
        return f.read()


def golden(name):
    path = os.path.join(GOLDEN, name + ".out.gz")
    if not os.path.exists(path):
        return None
    with gzip.open(path, "rb") as f:
        return f.read()


# ---- golden + decode --------------------------------------------------------

def test_pages():
    case_dir = os.path.join(WORK, "cases")
    out_dir = os.path.join(WORK, "out")
    os.makedirs(out_dir, exist_ok=True)
    run([sys.executable, os.path.join(HERE, "gen_cases.py"), case_dir])
    cases = load_cases(case_dir)
    have_golden = os.path.isdir(GOLDEN)
    if not have_golden:
        print("note: tests/golden is missing; skipping the vendor comparison")

    smoothed = {}  # image path -> horizontally doubled image from the driver
    for case in cases:
        kind, name = case[0], case[1]
        out = os.path.join(out_dir, name + ".out")

        native_output(TOOL, case, out)
        if kind == "smooth":
            smoothed[case[4]] = read(out)

        if have_golden and not name.startswith("own-"):
            want = golden(name)
            if want is None:
                check("golden", name, False, "no golden file")
            else:
                check("golden", name, read(out) == want,
                      "output differs from the vendor routine's")

    for case in cases:
        if case[0] != "page":
            continue
        _, name, mode, w, h, _hwres, margin, matrix_path, src = case
        w, h, margin = int(w), int(h), int(margin)
        matrix = read(matrix_path)
        pixels = read(src)

        if mode == "smooth":
            if src not in smoothed:
                run([TOOL, "smooth", str(w), str(h), src,
                     os.path.join(out_dir, name + ".x2")])
                smoothed[src] = read(os.path.join(out_dir, name + ".x2"))
            wide = smoothed[src]
            ew = (2 * w + 3) & ~3
            doubled = bytearray()
            for y in range(h):
                doubled += wide[y * ew:(y + 1) * ew] * 2
            want_rows = halftone(bytes(doubled), ew, 2 * h, matrix)
        else:
            want_rows = halftone(pixels, w, h, matrix)

        try:
            rows = brdecode.decode_blocks(read(os.path.join(out_dir, name + ".out")))
        except brdecode.FormatError as err:
            check("decode", name, False, str(err))
            continue

        nbytes = len(want_rows[0])
        ok = len(rows) == len(want_rows)
        detail = "%d rows decoded, %d expected" % (len(rows), len(want_rows))
        if ok:
            for y, (got, want) in enumerate(zip(rows, want_rows)):
                got = got.ljust(margin + nbytes, b"\0")
                if got != b"\0" * margin + want:
                    ok, detail = False, "row %d differs" % y
                    break
        check("decode", name, ok, detail)


# ---- job framing ------------------------------------------------------------

EXPECTED_HEADER = (
    "\x1b%%-12345X@PJL\n"
    "@PJL SET HOLD=OFF\n"
    "@PJL SET JOBNAME=\"Report 'Q3'\"\n"
    "@PJL SET USERNAME=\"alex\"\n"
    "@PJL JOB NAME=\"Report 'Q3'\"\n"
    "@PJL PRINTLOG ITEM = 1,PRINTER\n"
    "@PJL PRINTLOG ITEM = 2,Tue,24 Nov 2026 1:2:3\n"
    "@PJL PRINTLOG ITEM = 3,alex\n"
    "@PJL PRINTLOG ITEM = 4,Macintosh\n"
    "@PJL SET STRINGCODESET=HPROMAN8\n"
    "@PJL SET LOGINUSER=\"alex\"\n"
    "%(resolution)s"
    "@PJL SET ECONOMODE = %(economode)s\n"
    "@PJL SET LESSPAPERCURL=%(curl)s\n"
    "@PJL SET FIXINTENSITYUP=%(fixing)s\n"
    "@PJL SET TRANSFERLEVELUP=ON\n"
    "@PJL TRANSFERLEVEL=0\n"
    "@PJL SET MEDIATYPE=%(media)s\n"
    "%(density)s"
    "@PJL DEFAULT AUTOSLEEP = ON\n"
    "@PJL DEFAULT TIMEOUTSLEEP = 1\n"
    "@PJL SET AUTOSLEEP = ON\n"
    "@PJL SET TIMEOUTSLEEP = 1\n"
    "@PJL SET JOBTIME = \"20261124010203\"\n"
    "@PJL SET ORIENTATION = PORTRAIT\n"
    "@PJL SET PAPER = %(paper)s\n"
    "@PJL SET PAGEPROTECT = AUTO\n"
    "@PJL ENTER LANGUAGE = PCL\n"
    "\x1bE\x1b&l0S%(pagesize)s"
    "\x1b&l7H\x1b&l1X\x1b*b1030m\f"
    "\x1b%%-12345X@PJL\n"
    "@PJL EOJ NAME=\"Report 'Q3'\"\n"
    "\x1b%%-12345X"
)


def test_framing():
    epoch = "1795482123"  # 2026-11-24 01:02:03 UTC
    env = dict(os.environ, TZ="UTC")
    variants = [
        # name, res, text, toner, improve, density, media, w, l, expectations
        ("default-600", 1, 0, 0, 0, 99, 1, 612, 792,
         dict(resolution="@PJL SET RESOLUTION = 600\n", economode="OFF",
              curl="OFF", fixing="OFF", media="REGULAR", density="",
              paper="LETTER", pagesize="")),
        ("smooth-600", 2, 0, 1, 1, 99, 10, 595, 842,
         dict(resolution="@PJL SET RESOLUTION = 600\n", economode="ON",
              curl="ON", fixing="OFF", media="RECYCLED", density="",
              paper="A4", pagesize="")),
        ("draft-300", 0, 1, 0, 2, -3, 1, 612, 1008,
         dict(resolution="@PJL SET RESOLUTION = 300\n", economode="OFF",
              curl="OFF", fixing="ON", media="REGULAR",
              density="@PJL SET DENSITY = -3\n@PJL SET DEVBIASADJUST = -3\n",
              paper="LEGAL", pagesize="")),
        ("hq-1200", 3, 0, 0, 0, 6, 1, 595, 765,
         dict(resolution="@PJL SET RESOLUTION = 1200\n"
                         "@PJL SET PAPERFEEDSPEED = FULL\n", economode="OFF",
              curl="OFF", fixing="OFF", media="REGULAR",
              density="@PJL SET DENSITY = 6\n@PJL SET DEVBIASADJUST = 6\n",
              paper="270MM", pagesize="\x1b&l1043A")),
    ]
    for name, res, text, toner, improve, density, media, w, l, expect in variants:
        got = run([TOOL, "header", str(res), str(text), str(toner), str(improve),
                   str(density), str(media), str(w), str(l), epoch, "alex",
                   "Report \"Q3\""], env=env).stdout
        want = (EXPECTED_HEADER % expect).encode("latin-1")
        check("framing", name, got == want, "job framing differs")


# ---- paper table --------------------------------------------------------------

def test_paper():
    expected = {
        (595, 842): "A4 0", (612, 792): "LETTER 0", (612, 1008): "LEGAL 0",
        (522, 756): "EXECUTIVE 0", (420, 595): "A5 0", (595, 420): "A5L 0",
        (499, 709): "B5 0", (516, 729): "JISB5 0", (612, 936): "FOLIO 0",
        (595, 765): "270MM 0", (553, 765): "SIXTEENK195X270 0",
        (522, 737): "SIXTEENK184X260 0", (558, 774): "ROC16K 0",
        (500, 700): "JISB5 16",     # custom size -> smallest paper that holds it
        (420, 600): "B5 79",
        (612, 1000): "LEGAL 0",
    }
    for (w, l), want in expected.items():
        got = run([TOOL, "paper", str(w), str(l)]).stdout.decode().strip()
        check("paper", "%dx%d" % (w, l), got == want, "got %r, want %r" % (got, want))


# ---- the filter itself --------------------------------------------------------

def raster_header(width, height, dpi, colorspace=3, bpc=8, integers=(),
                  media_type=1, copies=1, page=(612, 792), margin=12):
    h = bytearray(1796)

    def put(offset, value):
        struct.pack_into("<I", h, offset, value)

    put(276, dpi)
    put(280, dpi)
    put(312, margin)
    put(316, margin)
    put(340, copies)
    put(352, page[0])
    put(356, page[1])
    put(372, width)
    put(376, height)
    put(380, media_type)
    put(384, bpc)
    put(388, bpc)
    put(392, (width * bpc + 7) // 8)
    put(400, colorspace)
    put(420, 1)
    for i, value in enumerate(integers):
        put(452 + 4 * i, value)
    return bytes(h)


def write_raster(path, pages):
    """pages: list of (header bytes, pixel bytes)."""
    with open(path, "wb") as f:
        f.write(struct.pack("<I", 0x52615333))  # "RaS3": uncompressed
        for header, pixels in pages:
            f.write(header)
            f.write(pixels)


def run_filter(raster, title="job"):
    proc = subprocess.run([FILTER, "7", "pat", title, "1", "", raster],
                          capture_output=True)
    return proc.returncode, proc.stdout, proc.stderr.decode("utf-8", "replace")


def test_filter():
    out_dir = os.path.join(WORK, "filter")
    os.makedirs(out_dir, exist_ok=True)

    w, h = 301, 140
    art = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            if 20 <= y < 60:
                art[y * w + x] = (x * 255) // (w - 1)
            elif 70 <= y < 120 and (x // 20 + y // 10) % 2 == 0:
                art[y * w + x] = 255
    art = bytes(art)
    blank = bytes(w * h)

    def screen(index):
        path = os.path.join(out_dir, "screen%d.bin" % index)
        run([TOOL, "screen", str(index), path])
        return read(path)

    # 1. 600 dpi, graphics screen, three pages with a blank one in the middle.
    ras = os.path.join(out_dir, "plain.ras")
    hdr = raster_header(w, h, 600, copies=2)
    write_raster(ras, [(hdr, art), (hdr, blank), (hdr, art)])
    rc, out, err = run_filter(ras)
    ok = rc == 0
    detail = "exit %d: %s" % (rc, err.strip().splitlines()[-1] if err.strip() else "")
    if ok:
        try:
            job = brdecode.parse_job(out)
            want = halftone(art, w, h, screen(2))
            ok = (len(job.pages) == 3 and
                  "@PJL SET RESOLUTION = 600" in job.pjl and
                  "@PJL SET PAPER = LETTER" in job.pjl and
                  job.pages[0].commands == ["&l7H", "&l2X"] and
                  job.pages[1].commands == ["&l2X"] and
                  [r.ljust(3 + len(want[0]), b"\0") for r in job.pages[0].rows] ==
                  [b"\0\0\0" + r for r in want] and
                  all(not any(r) for r in job.pages[1].rows) and
                  job.pages[2].rows == job.pages[0].rows and
                  err.count("PAGE: ") == 3)
            detail = "decoded job does not match"
        except brdecode.FormatError as e:
            ok, detail = False, str(e)
    check("filter", "600dpi-3-pages", ok, detail)

    # 2. Skip blank pages, text screen, toner save, 300 dpi.
    ras = os.path.join(out_dir, "skip.ras")
    hdr = raster_header(w, h, 300, integers=(0, 1, 1, 0, 1, 0))
    write_raster(ras, [(hdr, blank), (hdr, art), (hdr, blank)])
    rc, out, err = run_filter(ras)
    ok = rc == 0
    detail = "exit %d" % rc
    if ok:
        try:
            job = brdecode.parse_job(out)
            want = halftone(art, w, h, screen(1))
            ok = (len(job.pages) == 1 and
                  "@PJL SET RESOLUTION = 300" in job.pjl and
                  "@PJL SET ECONOMODE = ON" in job.pjl and
                  job.pages[0].commands == ["&l7H", "&l1X"] and
                  [r.ljust(2 + len(want[0]), b"\0") for r in job.pages[0].rows] ==
                  [b"\0\0" + r for r in want] and
                  err.count("PAGE: ") == 1)
            detail = "decoded job does not match"
        except brdecode.FormatError as e:
            ok, detail = False, str(e)
    check("filter", "skip-blank-300dpi", ok, detail)

    # 3. Smoothed 600 dpi from a 300 dpi rendering: twice the rows.
    ras = os.path.join(out_dir, "smooth.ras")
    hdr = raster_header(w, h, 300, integers=(1,))
    write_raster(ras, [(hdr, art)])
    rc, out, err = run_filter(ras)
    ok = rc == 0
    detail = "exit %d" % rc
    if ok:
        try:
            job = brdecode.parse_job(out)
            ok = ("@PJL SET RESOLUTION = 600" in job.pjl and
                  len(job.pages) == 1 and len(job.pages[0].rows) == 2 * h and
                  job.pages[0].width_bytes == 3 + ((2 * w + 3) // 4 * 4 + 7) // 8)
            detail = "decoded job does not match"
        except brdecode.FormatError as e:
            ok, detail = False, str(e)
    check("filter", "smooth-600dpi", ok, detail)

    # 4. White-is-255 input (sGray) is inverted.
    ras = os.path.join(out_dir, "sgray.ras")
    hdr = raster_header(w, h, 600, colorspace=18)
    write_raster(ras, [(hdr, bytes(255 - b for b in art))])
    rc, out2, err = run_filter(ras)
    ras = os.path.join(out_dir, "k.ras")
    write_raster(ras, [(raster_header(w, h, 600), art)])
    rc2, out3, _ = run_filter(ras)
    ok = rc == 0 and rc2 == 0
    if ok:
        ok = (brdecode.parse_job(out2).pages[0].rows ==
              brdecode.parse_job(out3).pages[0].rows)
    check("filter", "sgray-input", ok, "sGray and K input disagree")

    # 5. Things the filter must refuse.
    ras = os.path.join(out_dir, "onebit.ras")
    write_raster(ras, [(raster_header(w, h, 600, bpc=1), bytes(((w + 7) // 8) * h))])
    rc, out, err = run_filter(ras)
    check("filter", "rejects-1-bit", rc != 0 and "ERROR:" in err, "exit %d" % rc)

    ras = os.path.join(out_dir, "dpi.ras")
    write_raster(ras, [(raster_header(w, h, 720), art)])
    rc, out, err = run_filter(ras)
    check("filter", "rejects-720dpi", rc != 0 and "ERROR:" in err, "exit %d" % rc)

    ras = os.path.join(out_dir, "short.ras")
    write_raster(ras, [(raster_header(w, h, 600), art[:1000])])
    rc, out, err = run_filter(ras)
    check("filter", "rejects-truncated", rc != 0 and "ERROR:" in err, "exit %d" % rc)

    proc = subprocess.run([FILTER], capture_output=True)
    check("filter", "usage", proc.returncode != 0, "exit %d" % proc.returncode)


# ---- PPD ----------------------------------------------------------------------

def test_ppd():
    tool = shutil.which("cupstestppd")
    ppd = os.path.join(BUILD, "HL1210W.ppd")
    if not tool:
        print("note: cupstestppd not found; skipping the PPD check")
        return
    proc = subprocess.run([tool, "-W", "filters", "-W", "sizes", ppd],
                          capture_output=True, text=True)
    check("ppd", "cupstestppd", proc.returncode == 0 and "PASS" in proc.stdout,
          proc.stdout.strip()[-400:])


def main():
    for needed in (TOOL, FILTER):
        if not os.path.exists(needed):
            sys.exit("%s is missing; run 'make' first" % needed)
    os.makedirs(WORK, exist_ok=True)

    test_pages()
    test_framing()
    test_paper()
    test_filter()
    test_ppd()

    groups = {}
    for group, _name, ok, _detail in results:
        passed, total = groups.get(group, (0, 0))
        groups[group] = (passed + (1 if ok else 0), total + 1)
    for group, (passed, total) in groups.items():
        print("%-8s %d/%d passed" % (group, passed, total))
    failed = sum(1 for r in results if not r[2])
    print("ALL TESTS PASSED" if not failed else "%d TEST(S) FAILED" % failed)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
