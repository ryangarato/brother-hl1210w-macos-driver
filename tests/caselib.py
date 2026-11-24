"""Shared by run_tests.py and compare_original.py: read the generated test
cases and produce the native driver's output for one of them."""
import os
import subprocess

# Vendor PPD resolution choice -> br_resolution value of the native driver.
RESOLUTION = {"300dpi": 0, "600dpi": 1, "300x600dpi": 2, "2400x600dpi": 3}
OUTPUT_DPI = {"300dpi": 300, "600dpi": 600, "300x600dpi": 600, "2400x600dpi": 1200}

MARGIN_MARK = b"\n--left margin in bytes: "


def load_cases(case_dir):
    with open(os.path.join(case_dir, "cases.tsv")) as f:
        return [line.rstrip("\n").split("\t") for line in f if line.strip()]


def native_output(tool, case, out_path):
    """Run the native code for a case and write its output to out_path."""
    kind = case[0]
    if kind == "page":
        _, _, mode, w, h, _hwres, margin, matrix, src = case
        subprocess.run([tool, "page", mode, w, h, margin, matrix, src, out_path],
                       check=True)
    elif kind == "smooth":
        _, _, w, h, src = case
        subprocess.run([tool, "smooth", w, h, src, out_path], check=True)
    elif kind == "margin":
        _, _, left, extra, dpi = case
        text = subprocess.run([tool, "margin", left, extra, dpi], check=True,
                              capture_output=True).stdout
        with open(out_path, "wb") as f:
            f.write(text)
    elif kind == "job":
        (_, _, vres, text, toner, improve, density, media, width, length,
         epoch, user, title) = case
        env = dict(os.environ, TZ="UTC")
        framing = subprocess.run(
            [tool, "header", str(RESOLUTION[vres]), text, toner, improve,
             density, media, width, length, epoch, user, title],
            check=True, capture_output=True, env=env).stdout
        extra = subprocess.run([tool, "paper", width, length], check=True,
                               capture_output=True).stdout.split()[1].decode()
        margin = subprocess.run(
            [tool, "margin", "12", extra, str(OUTPUT_DPI[vres])], check=True,
            capture_output=True).stdout
        with open(out_path, "wb") as f:
            f.write(framing + MARGIN_MARK + margin)
    else:
        raise ValueError("unknown case kind %r" % kind)


def vendor_output(case, out_path):
    """Read what the emulated vendor code wrote for a case."""
    with open(out_path, "rb") as f:
        data = f.read()
    if case[0] == "job":
        with open(out_path + ".margin", "rb") as f:
            data += MARGIN_MARK + f.read()
    return data
