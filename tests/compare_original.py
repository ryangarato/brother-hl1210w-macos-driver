#!/usr/bin/env python3
"""Compare the native driver with Brother's original filter, routine by routine.

The original filter is Intel-only and cannot run on Apple silicon without
Rosetta, so its routines are executed in Ghidra's emulator
(tests/ghidra/EmuOriginal.java) on synthetic pages; the native code processes
the same pages and the outputs must be identical.

Usage:
  compare_original.py --binary PATH/rastertobrother1210
                      [--ghidra PATH/analyzeHeadless] [--work DIR]
                      [--only NAME ...] [--kind KIND ...] [--save-golden DIR]

With --save-golden the vendor outputs are stored (gzip-compressed) so that
"make check" can repeat the comparison later without Ghidra or the original.
"""
import argparse
import gzip
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from caselib import load_cases, native_output, vendor_output  # noqa: E402


def find_ghidra():
    for candidate in (shutil.which("analyzeHeadless"),
                      "/opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless"):
        if candidate and os.path.exists(candidate):
            return candidate
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--ghidra", default=find_ghidra())
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "compare"))
    ap.add_argument("--tool", default=os.path.join(ROOT, "build", "hl1210w-testtool"))
    ap.add_argument("--only", nargs="*", help="case names to run")
    ap.add_argument("--kind", nargs="*", help="case kinds to run "
                    "(page, smooth, margin, job)")
    ap.add_argument("--save-golden")
    args = ap.parse_args()

    if not args.ghidra:
        sys.exit("analyzeHeadless (Ghidra) not found; pass --ghidra")

    case_dir = os.path.join(args.work, "cases")
    vendor_dir = os.path.join(args.work, "vendor")
    native_dir = os.path.join(args.work, "native")
    for d in (case_dir, vendor_dir, native_dir):
        os.makedirs(d, exist_ok=True)

    subprocess.run([sys.executable, os.path.join(HERE, "gen_cases.py"),
                    case_dir], check=True)
    # "own-" cases cover behaviour that deliberately differs from the vendor.
    cases = [c for c in load_cases(case_dir) if not c[1].startswith("own-")]
    if args.only:
        cases = [c for c in cases if c[1] in args.only]
    if args.kind:
        cases = [c for c in cases if c[0] in args.kind]

    # Manifest for the emulator: same fields minus the name, plus the output.
    manifest = os.path.join(args.work, "manifest.tsv")
    with open(manifest, "w") as f:
        for case in cases:
            out = os.path.join(vendor_dir, case[1] + ".out")
            for stale in (out, out + ".margin"):
                if os.path.exists(stale):
                    os.remove(stale)
            f.write("\t".join([case[0]] + case[2:] + [out]) + "\n")

    project_dir = os.path.join(args.work, "ghidra-project")
    shutil.rmtree(project_dir, ignore_errors=True)
    os.makedirs(project_dir)
    log = os.path.join(args.work, "ghidra.log")
    with open(log, "w") as logf:
        proc = subprocess.run(
            [args.ghidra, project_dir, "hl1210w", "-import", args.binary,
             "-noanalysis", "-scriptPath", os.path.join(HERE, "ghidra"),
             "-postScript", "EmuOriginal.java", manifest, "-deleteProject"],
            stdout=logf, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        sys.exit("Ghidra failed; see " + log)

    failures = 0
    for case in cases:
        name = case[1]
        vendor = os.path.join(vendor_dir, name + ".out")
        native = os.path.join(native_dir, name + ".out")
        if not os.path.exists(vendor):
            print("MISSING  %-18s (the emulator produced nothing; see %s)" % (name, log))
            failures += 1
            continue
        native_output(args.tool, case, native)
        a = vendor_output(case, vendor)
        with open(native, "rb") as f:
            b = f.read()
        if a == b:
            print("same     %-18s %8d bytes" % (name, len(a)))
            if args.save_golden:
                os.makedirs(args.save_golden, exist_ok=True)
                with open(os.path.join(args.save_golden, name + ".out.gz"), "wb") as raw:
                    with gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as gz:
                        gz.write(a)
        else:
            first = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y),
                         min(len(a), len(b)))
            print("DIFFERS  %-18s vendor %d bytes, native %d bytes, first "
                  "difference at offset %d" % (name, len(a), len(b), first))
            failures += 1

    print("%d of %d cases identical" % (len(cases) - failures, len(cases)))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
