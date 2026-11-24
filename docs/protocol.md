# What the HL-1210W expects on the wire

These notes record what was learned from Brother's own macOS filter,
`rastertobrother1210` 4.0.0 (from *Brother_PrinterDrivers_MonochromeLaser* 1.5.0,
SHA-256 `11846ffc…9c9e8f`, x86_64 only), by static analysis with Ghidra. Function
names are the symbols in that binary. Everything here was then confirmed by
running the original routines in an emulator against this driver's output
(see [Verification](#verification)).

Nothing in this driver is copied from the vendor's code; it is a new
implementation of the same data format.

## Pipeline in the vendor filter

`main` → `SendRasterDoc` reads CUPS raster pages and drives a function table
(`SetFunctionTable`): `MBeginJob`, `MBeginPage`, `MSendBandData`, `MEndPage`,
`MEndJob`. A whole page is read as a single band.

| Step | Vendor routine | Here |
| --- | --- | --- |
| Job header (PJL) | `MBeginJob`, `SecureFunc`, `StartJobLogFunc`, `FunctionLock`, `GetDensityAdjustment`, `GetSleepMode` | `src/job.c` |
| Paper name, centring | `sortPaperResult`, `checkSendCUPSPaper` | `br_paper_for_size` |
| Left margin | `GetMargins` | `br_left_margin_bytes` |
| 300 → 600 dpi smoothing | `expandXaxis` / `ExpandSurfImage_Interpolate`, `expandYaxis` | `src/smooth.c` |
| Halftoning | `SetHalftoneMatrix`, `HalftonePixMap` | `src/halftone.c` |
| Line compression | inline in `MSendBandData`, `checkOptByte` | `br_encode_line` |
| Blocks | `SaveDumb`, `MSendData` | `br_page_*` |
| Page / job end | `MBeginPage`, `EndPageCommand`, `MEndJob` | `src/job.c` |

## Job framing

```
ESC %-12345X @PJL
@PJL SET HOLD=OFF
@PJL SET JOBNAME="<title>"
@PJL SET USERNAME="<user>"
@PJL JOB NAME="<title>"
@PJL PRINTLOG ITEM = 1,PRINTER
@PJL PRINTLOG ITEM = 2,<Day>,<d> <Mon> <yyyy> <h>:<m>:<s>      (UTC, unpadded)
@PJL PRINTLOG ITEM = 3,<user>
@PJL PRINTLOG ITEM = 4,Macintosh
@PJL SET STRINGCODESET=HPROMAN8
@PJL SET LOGINUSER="<user>"
@PJL SET RESOLUTION = 300 | 600 | 1200
@PJL SET PAPERFEEDSPEED = FULL                                  (1200 only)
@PJL SET ECONOMODE = ON | OFF
@PJL SET LESSPAPERCURL=ON | OFF
@PJL SET FIXINTENSITYUP=ON | OFF
@PJL SET TRANSFERLEVELUP=ON
@PJL TRANSFERLEVEL=0
@PJL SET MEDIATYPE=REGULAR | RECYCLED | ...
@PJL SET DENSITY = <n>                                          (only when set)
@PJL SET DEVBIASADJUST = <n>
@PJL DEFAULT AUTOSLEEP = ON
@PJL DEFAULT TIMEOUTSLEEP = 1
@PJL SET AUTOSLEEP = ON
@PJL SET TIMEOUTSLEEP = 1
@PJL SET JOBTIME = "<yyyymmddhhmmss>"                           (local time)
@PJL SET ORIENTATION = PORTRAIT
@PJL SET PAPER = <name>
@PJL SET PAGEPROTECT = AUTO
@PJL ENTER LANGUAGE = PCL
ESC E   ESC &l0S   [ESC &l1043A for 210 x 270 mm paper]

per page:   [ESC &l7H on the first page]  ESC &l<copies>X  ESC *b1030m  <blocks>  FF

ESC %-12345X @PJL
@PJL EOJ NAME="<title>"
ESC %-12345X
```

The sleep-timer lines are what the vendor filter sends for models whose PPD has
no sleep-time option, which includes this one.

Title and user are cut to 80 characters. The vendor filter passes them through
untouched; this driver additionally replaces control characters, double quotes
and non-ASCII bytes, any of which could otherwise break the PJL line.

### Paper names

`A4`, `LETTER`, `LEGAL`, `EXECUTIVE`, `A5`, `A5L` (A5 long edge), `B5` (ISO),
`JISB5`, `FOLIO`, `270MM`, `ROC16K` (197 × 273 mm), `SIXTEENK184X260`,
`SIXTEENK195X270`. The printer is told the first paper, ordered by width then
length, that is at least as wide and as long as the page; a narrower custom page
is centred on it by adding half the width difference to the left margin.

### Left margin

Rows start `margin` bytes in, where, with the left edge of the imageable area
`L` and half the centring slack `E/2` in points:

```
dots   = (L + E/2) * dpi / 72 - engine_offset      engine_offset = 40 / 80 / 160 dots
margin = (dots + 7) / 8                             at 300 / 600 / 1200 dpi
```

That is 2, 3 and 5 bytes for the standard 12-point margin. The margin is sent as
the first command of every line: "repeat 0x00 `margin` times".

## Raster data: compression mode 1030

After `ESC *b1030m` the page is a run of blocks, then a form feed:

```
<size + 2> 'w'   0x00   <lines>   <size bytes of line records>
```

A block is closed before it would exceed 0x4000 bytes of records and after every
128 lines (counted from the top of the page, blank lines included).

Each line record is one of

| Bytes | Meaning |
| --- | --- |
| `FF` | white line |
| `00` | same as the previous line |
| `n` + n commands | edit the previous line, left to right (`n` ≤ 254) |

and each command first skips `offset` bytes of the previous line, then writes:

| Header | Then | Meaning |
| --- | --- | --- |
| `0oooolll` | [offset ext] [count ext] `count` bytes | substitute `count = lll + 1` bytes |
| `1oolllll` | [offset ext] [count ext] one byte | repeat it `count = lllll + 2` times |

A field that is all ones continues in extension bytes: every `FF` adds 255 and
the first other byte adds its value and ends the field (a lone `00` when nothing
is left to add).

Encoder decisions that the firmware evidently relies on, or that keep the stream
identical to the vendor's:

* The first non-blank line at a multiple of 128 lines is coded against its own
  inverse, i.e. completely, never against the line before.
* A run of differing bytes shorter than 3 is one substitute. A longer run is
  split into repeats (3 or more equal bytes) and substitutes; only the first
  command of the run carries the offset.
* Once a line has used 253 commands, whatever remains is sent as one substitute.

Rows are `ceil(width / 8)` bytes, most significant bit on the left, 1 = black.

## Halftoning

8-bit black (CUPS colour space K) is thresholded against a 32 × 32 matrix tiled
over the page from its top-left corner: a pixel prints when its value is greater
than the threshold, a value of 255 always prints. The vendor's matrices ship as
`Brother LUT/<res>-<G|T>12.bin` (`"BRML"`, two 4-digit sizes, 1024 bytes) with a
copy inside the filter; `G` is the "Graphics" screen and `T` the "Text" one.

## "600 dpi" in the vendor PPD

Brother's PPD for this model renders at 300 dpi for its "600 dpi" choice
(`300x600dpi`) and enlarges in the filter: every pixel is doubled, and at the
inner corners of diagonal black edges one half-pixel is filled in and carried
along the edge (`ExpandSurfImage_Interpolate`). This driver keeps that mode as
"600 dpi (Fast)" and adds a true 600 dpi rendering, which the same code path in
the vendor filter also supports (`600dpi`), as the default.

## Not carried over

* Brother's status monitor (`commandtobrother`, PJL `USTATUS` back-channel and
  private SNMP queries). CUPS's own backends report supply levels and errors
  from the standard printer MIB instead (`*cupsSNMPSupplies: True`).
* Secure print, fax, driver-side duplex and 180° rotation: paths in the shared
  vendor filter that this model's PPD never enables.
* The custom print-dialog panel; every setting is an ordinary PPD option.

## Verification

`tests/compare_original.py` runs the vendor's own routines in Ghidra's p-code
emulator (no Rosetta needed; library calls are served by small hooks in
`tests/ghidra/EmuOriginal.java`) and compares the bytes with this driver's:

* `MSendBandData` + `MSendData` on synthetic pages: halftoning, smoothing,
  compression and block framing, including lines with more than 253 commands,
  16 KB block flushes, 128-line boundaries and large margins;
* `ExpandSurfImage_Interpolate` on its own;
* `GetMargins`;
* `sortPaperResult`, `MBeginJob`, `MBeginPage`, `MEndPage` and `MEndJob`: the
  complete job framing for every paper size at every resolution, plus mixes of
  the other options.

The captured outputs live in `tests/golden`, so `make check` repeats the
comparison anywhere. A separate decoder (`tests/brdecode.py`), written from the
format description above, checks that every stream decodes to the expected
bitmap.

One deliberate difference: a left margin of exactly one byte cannot be written
as a "repeat" command, and the vendor code emits a malformed command for it.
That margin never occurs with this printer's page sizes; this driver writes it
as a one-byte substitute instead (test case `own-margin1`).
