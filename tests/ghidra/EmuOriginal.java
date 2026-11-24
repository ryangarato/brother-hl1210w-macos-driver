// Runs routines of Brother's original (Intel-only) rastertobrother1210 filter
// inside Ghidra's p-code emulator, so that the native driver's output can be
// compared with the vendor's byte for byte on machines that cannot execute
// x86_64 code.  Nothing is patched; library calls are served by small hooks.
//
// Script argument: a manifest file, one test per line:
//
//   page   <native|smooth> W H HWRES MARGIN MATRIX IN OUT
//       MSendBandData + MSendData on an 8-bit page -> compressed blocks
//   smooth W H IN OUT
//       ExpandSurfImage_Interpolate -> horizontally doubled page
//   margin LEFT_PT EXTRA_PT DPI OUT
//       GetMargins -> decimal margin in bytes
//   job    VRES TEXT TONER IMPROVE DENSITY MEDIA WIDTH_PT LENGTH_PT EPOCH USER TITLE OUT
//       sortPaperResult, MBeginJob, MBeginPage, MEndPage, MEndJob for an empty
//       first page -> the complete job framing; "<OUT>.margin" gets the left
//       margin the vendor computed.  VRES is the vendor PPD's resolution
//       choice (300dpi, 300x600dpi, 600dpi or 2400x600dpi), DENSITY 99 means
//       "printer default".  Times are taken as UTC.
//
//@category HL1210W

import java.io.ByteArrayOutputStream;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.time.Instant;
import java.time.ZoneOffset;
import java.time.ZonedDateTime;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import ghidra.app.emulator.EmulatorHelper;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class EmuOriginal extends GhidraScript {

	// Routines of rastertobrother1210 4.0.0 (sha256 11846ffc...9c9e8f).
	static final long FN_MSendBandData = 0x10000491cL;
	static final long FN_MSendData = 0x1000054bcL;
	static final long FN_Interpolate = 0x100008904L;
	static final long FN_GetMargins = 0x1000034e5L;
	static final long FN_sortPaperResult = 0x10000230eL;
	static final long FN_MBeginJob = 0x100003858L;
	static final long FN_MBeginPage = 0x1000045feL;
	static final long FN_MEndPage = 0x100004898L;
	static final long FN_MEndJob = 0x10000458fL;

	// Import stubs that the routines above reach.
	static final long STUB_bzero = 0x10000d95eL;
	static final long STUB_stack_chk_fail = 0x10000d96aL;
	static final long STUB_fprintf = 0x10000d98eL;
	static final long STUB_free = 0x10000d994L;
	static final long STUB_fwrite = 0x10000d99aL;
	static final long STUB_malloc = 0x10000d9b2L;
	static final long STUB_memcpy = 0x10000d9b8L;
	static final long STUB_memset = 0x10000d9beL;
	static final long STUB_printf = 0x10000d9d0L;
	static final long STUB_cupsGetOption = 0x10000d8feL;
	static final long STUB_ppdFindAttr = 0x10000d91cL;
	static final long STUB_ppdFindCustomOption = 0x10000d928L;
	static final long STUB_ppdFindCustomParam = 0x10000d92eL;
	static final long STUB_ppdFindOption = 0x10000d934L;
	static final long STUB_atoi = 0x10000d970L;
	static final long STUB_close = 0x10000d97cL;
	static final long STUB_fflush = 0x10000d988L;
	static final long STUB_gmtime = 0x10000d9a6L;
	static final long STUB_localtime = 0x10000d9acL;
	static final long STUB_open = 0x10000d9c4L;
	static final long STUB_sprintf = 0x10000da0cL;
	static final long STUB_strcat = 0x10000da18L;
	static final long STUB_strcmp = 0x10000da1eL;
	static final long STUB_strcpy = 0x10000da24L;
	static final long STUB_strlen = 0x10000da2aL;
	static final long STUB_strstr = 0x10000da3cL;
	static final long STUB_time = 0x10000da42L;

	// Globals.
	static final long GOT_stack_chk_guard = 0x100012028L;
	static final long GOT_stderrp = 0x100012030L;
	static final long GOT_stdoutp = 0x100012038L;
	static final long G_Planes = 0x1000156c0L;
	static final long G_thisPPD = 0x1000156f8L;
	static final long G_gLoginUserName = 0x100016838L;
	static final long G_gLogJobName = 0x100016840L;
	static final long G_fun_MSendData = 0x100016830L;
	static final long G_pMatrixRow = 0x100017050L;
	static final long G_lMatrixRow = 0x100017058L;
	static final long G_margins = 0x100017060L;
	static final long G_send_buf = 0x100017070L;
	static final long G_comp_out = 0x1000170a8L;
	static final long G_comp_cmd = 0x1000170c8L;
	static final long G_model_language = 0x1000170d0L;

	static final long BSS_START = 0x1000156a0L;
	static final long BSS_END = 0x100018000L;

	static final long HEAP_BASE = 0x200000000L;
	static final long STACK_TOP = 0x300100000L;
	static final long SENTINEL = 0x3f0000000L;
	static final long STDOUT_FILE = 0x3e0000010L;
	static final long STDERR_FILE = 0x3e0000020L;

	private EmulatorHelper emu;
	private long heap;
	private ByteArrayOutputStream stdout;
	private Map<String, String> jobOptions = new HashMap<>();
	private Map<String, Long> optionStrings = new HashMap<>();
	private long epoch;
	private long densityParam;

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length != 1) {
			throw new IllegalArgumentException("usage: EmuOriginal.java <manifest>");
		}
		List<String> lines = Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8);
		int n = 0;
		for (String line : lines) {
			line = line.trim();
			if (line.isEmpty() || line.startsWith("#")) {
				continue;
			}
			String[] f = line.split("\t");
			long t0 = System.currentTimeMillis();
			runCase(f);
			println("case " + (++n) + " [" + f[0] + "] done in " +
				(System.currentTimeMillis() - t0) + " ms");
		}
	}

	private void runCase(String[] f) throws Exception {
		emu = new EmulatorHelper(currentProgram);
		try {
			reset();
			switch (f[0]) {
				case "page":
					casePage(f);
					break;
				case "smooth":
					caseSmooth(f);
					break;
				case "margin":
					caseMargin(f);
					break;
				case "job":
					caseJob(f);
					break;
				default:
					throw new IllegalArgumentException("unknown case " + f[0]);
			}
		}
		finally {
			emu.dispose();
		}
	}

	// ---- test cases ----------------------------------------------------

	private void casePage(String[] f) throws Exception {
		boolean smooth = f[1].equals("smooth");
		int w = Integer.parseInt(f[2]);
		int h = Integer.parseInt(f[3]);
		int hwres = Integer.parseInt(f[4]);
		int margin = Integer.parseInt(f[5]);
		byte[] matrix = Files.readAllBytes(Paths.get(f[6]));
		byte[] image = Files.readAllBytes(Paths.get(f[7]));
		if (image.length != w * h || matrix.length != 1024) {
			throw new IllegalArgumentException("bad input sizes for " + f[7]);
		}

		int ew = (2 * w + 3) & ~3; // GetExpandDataDelta(2 * cupsWidth)
		int outW = smooth ? ew : w;
		int outH = smooth ? 2 * h : h;

		// CreateBandBuf2 + cupsRasterReadPixels
		long planes = alloc((long) outW * outH + 64);
		write(planes, image);
		writeQ(G_Planes, planes);

		// MBeginJob's bookkeeping in the driver's per-job struct
		long sdata = alloc(0x40);
		writeW(sdata + 0x04, h);
		writeW(sdata + 0x14, smooth ? 0x10 : 0);
		writeW(sdata + 0x18, 2 * h);
		writeW(sdata + 0x1a, ew);
		writeW(sdata + 0x1c, w);
		writeW(sdata + 0x22, outW);
		writeW(sdata + 0x24, outH);

		// cups_page_header2_t fields the routine reads
		long header = alloc(1796);
		writeD(header + 0x114, hwres); // HWResolution[0]
		writeD(header + 0x174, w); // cupsWidth
		writeD(header + 0x178, h); // cupsHeight
		writeD(header + 0x180, 8); // cupsBitsPerColor
		writeD(header + 0x188, w); // cupsBytesPerLine
		writeD(header + 0x190, 3); // cupsColorSpace = K

		// SetFunctionTable, CreateSendBuf, AllocateCompMemory, GetMargins,
		// SetHalftoneMatrix
		writeD(G_model_language, 3);
		writeQ(G_fun_MSendData, FN_MSendData);
		writeQ(G_send_buf, alloc(0x4040));
		long comp = ((long) outW * 5) >> 3;
		writeQ(G_comp_out, alloc(comp + 64));
		writeQ(G_comp_cmd, alloc(comp + 64));
		writeW(G_margins, margin);
		writeW(G_margins + 2, margin);
		long m = alloc(1024);
		write(m, matrix);
		writeQ(G_pMatrixRow, m);
		writeQ(G_lMatrixRow, 32);

		long rc = call(FN_MSendBandData, header, sdata) & 0xffff;
		if (rc != 0) {
			throw new IllegalStateException("MSendBandData returned " + rc);
		}
		call(FN_MSendData); // EndPageCommand's FlushBuf
		Files.write(Paths.get(f[8]), stdout.toByteArray());
	}

	private void caseSmooth(String[] f) throws Exception {
		int w = Integer.parseInt(f[1]);
		int h = Integer.parseInt(f[2]);
		byte[] image = Files.readAllBytes(Paths.get(f[3]));
		int ew = (2 * w + 3) & ~3;

		long src = alloc((long) w * h + 64);
		write(src, image);
		long dst = alloc((long) ew * h + 64);

		// ExpandSurfImage_Interpolate(srcStride, src, dst, dstStride, height, width)
		long rc = call(FN_Interpolate, w, src, dst, ew, h, w) & 0xffff;
		if (rc != 0) {
			throw new IllegalStateException("ExpandSurfImage_Interpolate returned " + rc);
		}
		Files.write(Paths.get(f[4]), emu.readMemory(toAddr(dst), ew * h));
	}

	private void caseMargin(String[] f) throws Exception {
		int left = Integer.parseInt(f[1]);
		int extra = Integer.parseInt(f[2]);
		int dpi = Integer.parseInt(f[3]);
		long sdata = alloc(0x40);
		writeW(sdata + 0x1e, dpi);
		writeW(sdata + 0x20, left);
		call(FN_GetMargins, sdata, extra);
		short margin = (short) readW(G_margins);
		Files.write(Paths.get(f[4]),
			(Integer.toString(margin > 0 ? margin : 0) + "\n").getBytes(StandardCharsets.UTF_8));
	}

	private void caseJob(String[] f) throws Exception {
		String vres = f[1];
		boolean text = !f[2].equals("0");
		boolean toner = !f[3].equals("0");
		int improve = Integer.parseInt(f[4]);
		int density = Integer.parseInt(f[5]);
		int media = Integer.parseInt(f[6]);
		int pageW = Integer.parseInt(f[7]);
		int pageL = Integer.parseInt(f[8]);
		epoch = Long.parseLong(f[9]);
		String user = f[10];
		String title = f[11];

		int hwres = vres.equals("600dpi") ? 600 : vres.equals("2400x600dpi") ? 1200 : 300;

		// The job options as the vendor PPD and print dialog would deliver them.
		jobOptions.clear();
		optionStrings.clear();
		jobOptions.put("BRResolution", vres);
		jobOptions.put("BRHalfTonePattern", text ? "Brother4" : "Brother");
		jobOptions.put("TonerSaveMode", toner ? "ON" : "OFF");
		jobOptions.put("BRImproveOutput",
			improve == 1 ? "BRLessPaperCurl" : improve == 2 ? "BRFixIntensity" : "OFF");
		jobOptions.put("BRSkipBlank", "OFF");
		jobOptions.put("BRMediaType", media == 10 ? "Recycled" : "Plain");
		if (density == 99) {
			jobOptions.put("BRDensityAdjustment", "Default");
		}
		else {
			jobOptions.put("BRDensityAdjustment", "Custom");
			jobOptions.put("com.brother.print.PrintSettings.densitymode",
				Integer.toString(density));
		}

		// "*ParamCustomBRDensityAdjustment Density: 1 int -6 6"
		densityParam = alloc(0x100);
		writeD(densityParam + 0x84, -6);
		writeD(densityParam + 0x88, 6);

		// ppd_file_t with the vendor PPD's page sizes, in file order.
		String[] names = { "A4", "Letter", "Legal", "Executive", "A5", "A5Rotated", "ISOB5",
			"B5", "FanFoldGermanLegal", "210x270mm", "195x270mm", "184x260mm", "197x273mm",
			"Custom" };
		int[][] dims = { { 595, 842 }, { 612, 792 }, { 612, 1008 }, { 522, 756 }, { 420, 595 },
			{ 595, 420 }, { 499, 709 }, { 516, 729 }, { 612, 936 }, { 595, 765 }, { 553, 765 },
			{ 522, 737 }, { 558, 774 }, { 0, 0 } };
		long sizes = alloc(names.length * 0x48L);
		for (int i = 0; i < names.length; i++) {
			long sz = sizes + i * 0x48L;
			write(sz + 4, cstr(names[i]));
			writeD(sz + 0x30, Float.floatToRawIntBits(dims[i][0]));
			writeD(sz + 0x34, Float.floatToRawIntBits(dims[i][1]));
		}
		long ppd = alloc(0x400);
		writeD(ppd + 0x18, 9); // cupsModelNumber
		writeQ(ppd + 0x78, string("Brother"));
		writeQ(ppd + 0x88, string("Brother HL-1210W series CUPS"));
		writeQ(ppd + 0x90, string("HL-1210W series"));
		writeD(ppd + 0xa8, names.length);
		writeQ(ppd + 0xb0, sizes);
		writeQ(G_thisPPD, ppd);

		writeQ(G_gLoginUserName, string(user));
		writeQ(G_gLogJobName, string(title));

		int w = (pageW - 24) * hwres / 72;
		int h = (pageL - 24) * hwres / 72;
		long header = alloc(1796);
		writeD(header + 0x114, hwres);
		writeD(header + 0x118, hwres);
		writeD(header + 0x138, 12); // Margins[0]
		writeD(header + 0x154, 1); // NumCopies
		writeD(header + 0x160, pageW);
		writeD(header + 0x164, pageL);
		writeD(header + 0x174, w);
		writeD(header + 0x178, h);
		writeD(header + 0x17c, media);
		writeD(header + 0x180, 8);
		writeD(header + 0x188, w);
		writeD(header + 0x190, 3);

		long sdata = alloc(0x40);
		writeD(G_model_language, 3);
		writeQ(G_fun_MSendData, FN_MSendData);
		writeQ(G_send_buf, alloc(0x4040));
		writeQ(G_Planes, alloc(64));

		call(FN_sortPaperResult, ppd);
		expectZero("MBeginJob", call(FN_MBeginJob, header, ppd, sdata));
		expectZero("MBeginPage", call(FN_MBeginPage, header, sdata, 1));
		call(FN_MEndPage, header, sdata);
		expectZero("MEndJob", call(FN_MEndJob));

		Files.write(Paths.get(f[12]), stdout.toByteArray());
		short margin = (short) readW(G_margins);
		Files.write(Paths.get(f[12] + ".margin"),
			(Integer.toString(margin > 0 ? margin : 0) + "\n").getBytes(StandardCharsets.UTF_8));
	}

	private void expectZero(String what, long rc) {
		if ((rc & 0xffff) != 0) {
			throw new IllegalStateException(what + " returned " + (rc & 0xffff));
		}
	}

	// ---- emulator plumbing -----------------------------------------------

	private void reset() throws Exception {
		heap = HEAP_BASE;
		stdout = new ByteArrayOutputStream();

		emu.writeMemory(toAddr(BSS_START), new byte[(int) (BSS_END - BSS_START)]);
		emu.writeMemory(toAddr(STACK_TOP - 0x100000), new byte[0x100000 + 0x1000]);

		// ___stack_chk_guard, ___stderrp and ___stdoutp live in other images.
		long cells = alloc(0x40);
		writeQ(cells, 0x5ca1ab1e0badf00dL);
		writeQ(cells + 8, STDERR_FILE);
		writeQ(cells + 16, STDOUT_FILE);
		writeQ(GOT_stack_chk_guard, cells);
		writeQ(GOT_stderrp, cells + 8);
		writeQ(GOT_stdoutp, cells + 16);

		for (long stub : new long[] { STUB_bzero, STUB_stack_chk_fail, STUB_fprintf, STUB_free,
			STUB_fwrite, STUB_malloc, STUB_memcpy, STUB_memset, STUB_printf, STUB_cupsGetOption,
			STUB_ppdFindAttr, STUB_ppdFindCustomOption, STUB_ppdFindCustomParam,
			STUB_ppdFindOption, STUB_atoi, STUB_close, STUB_fflush, STUB_gmtime, STUB_localtime,
			STUB_open, STUB_sprintf, STUB_strcat, STUB_strcmp, STUB_strcpy, STUB_strlen,
			STUB_strstr, STUB_time, SENTINEL }) {
			emu.setBreakpoint(toAddr(stub));
		}
	}

	private long call(long fn, long... args) throws Exception {
		String[] regs = { "RDI", "RSI", "RDX", "RCX", "R8", "R9" };
		for (int i = 0; i < args.length; i++) {
			emu.writeRegister(regs[i], args[i]);
		}
		long rsp = STACK_TOP - 0x100;
		writeQ(rsp, SENTINEL);
		emu.writeRegister("RSP", rsp);
		emu.writeRegister("RBP", 0);

		long pc = fn;
		while (true) {
			if (monitor.isCancelled()) {
				throw new IllegalStateException("cancelled");
			}
			boolean atBreak = emu.run(toAddr(pc), null, monitor);
			long at = emu.getExecutionAddress().getOffset();
			if (!atBreak) {
				throw new IllegalStateException(
					"emulation stopped at " + Long.toHexString(at) + ": " + emu.getLastError());
			}
			if (at == SENTINEL) {
				return reg("RAX");
			}
			pc = hook(at);
		}
	}

	/** Serve a library call and return the address execution resumes at. */
	private long hook(long stub) throws Exception {
		long a0 = reg("RDI"), a1 = reg("RSI"), a2 = reg("RDX"), a3 = reg("RCX");
		long result = 0;

		if (stub == STUB_malloc) {
			result = alloc(a0);
		}
		else if (stub == STUB_free) {
			// nothing: allocations are never reused
		}
		else if (stub == STUB_bzero) {
			fill(a0, (byte) 0, a1);
		}
		else if (stub == STUB_memset) {
			fill(a0, (byte) a1, a2);
			result = a0;
		}
		else if (stub == STUB_memcpy) {
			if (a2 > 0) {
				write(a0, emu.readMemory(toAddr(a1), (int) a2));
			}
			result = a0;
		}
		else if (stub == STUB_fwrite) {
			if (a3 == STDOUT_FILE && a1 * a2 > 0) {
				stdout.write(emu.readMemory(toAddr(a0), (int) (a1 * a2)));
			}
			result = a2;
		}
		else if (stub == STUB_printf) {
			byte[] text = format(a0, new long[] { a1, a2, a3, reg("R8"), reg("R9") });
			stdout.write(text);
			result = text.length;
		}
		else if (stub == STUB_sprintf) {
			byte[] text = format(a1, new long[] { a2, a3, reg("R8"), reg("R9") });
			write(a0, java.util.Arrays.copyOf(text, text.length + 1));
			result = text.length;
		}
		else if (stub == STUB_fprintf) {
			byte[] text = format(a1, new long[] { a2, a3, reg("R8"), reg("R9") });
			println("fprintf: " + new String(text, StandardCharsets.ISO_8859_1).trim());
			result = text.length;
		}
		else if (stub == STUB_cupsGetOption) {
			String name = new String(cbytes(a0), StandardCharsets.ISO_8859_1);
			String value = jobOptions.get(name);
			if (value != null) {
				Long p = optionStrings.get(name);
				if (p == null) {
					p = string(value);
					optionStrings.put(name, p);
				}
				result = p;
			}
		}
		else if (stub == STUB_ppdFindOption || stub == STUB_ppdFindAttr) {
			result = 0; // the option or attribute is not in this model's PPD
		}
		else if (stub == STUB_ppdFindCustomOption || stub == STUB_ppdFindCustomParam) {
			result = densityParam;
		}
		else if (stub == STUB_strlen) {
			result = cbytes(a0).length;
		}
		else if (stub == STUB_strcmp) {
			byte[] x = cbytes(a0), y = cbytes(a1);
			int i = 0;
			while (i < x.length && i < y.length && x[i] == y[i]) {
				i++;
			}
			result = (i < x.length ? x[i] & 0xff : 0) - (i < y.length ? y[i] & 0xff : 0);
		}
		else if (stub == STUB_strcpy) {
			byte[] x = cbytes(a1);
			write(a0, java.util.Arrays.copyOf(x, x.length + 1));
			result = a0;
		}
		else if (stub == STUB_strcat) {
			byte[] x = cbytes(a1);
			write(a0 + cbytes(a0).length, java.util.Arrays.copyOf(x, x.length + 1));
			result = a0;
		}
		else if (stub == STUB_strstr) {
			String hay = new String(cbytes(a0), StandardCharsets.ISO_8859_1);
			int at = hay.indexOf(new String(cbytes(a1), StandardCharsets.ISO_8859_1));
			result = at < 0 ? 0 : a0 + at;
		}
		else if (stub == STUB_atoi) {
			String digits = new String(cbytes(a0), StandardCharsets.ISO_8859_1).trim();
			int end = 0;
			while (end < digits.length() && (Character.isDigit(digits.charAt(end)) ||
				(end == 0 && (digits.charAt(0) == '-' || digits.charAt(0) == '+')))) {
				end++;
			}
			String number = digits.substring(0, end);
			result = number.isEmpty() || number.equals("-") || number.equals("+") ? 0
					: Integer.parseInt(number.startsWith("+") ? number.substring(1) : number);
		}
		else if (stub == STUB_time) {
			if (a0 != 0) {
				writeQ(a0, epoch);
			}
			result = epoch;
		}
		else if (stub == STUB_gmtime || stub == STUB_localtime) {
			ZonedDateTime t = Instant.ofEpochSecond(readQ(a0)).atZone(ZoneOffset.UTC);
			long tm = alloc(64);
			writeD(tm, t.getSecond());
			writeD(tm + 4, t.getMinute());
			writeD(tm + 8, t.getHour());
			writeD(tm + 12, t.getDayOfMonth());
			writeD(tm + 16, t.getMonthValue() - 1);
			writeD(tm + 20, t.getYear() - 1900);
			writeD(tm + 24, t.getDayOfWeek().getValue() % 7);
			writeD(tm + 28, t.getDayOfYear() - 1);
			result = tm;
		}
		else if (stub == STUB_open) {
			result = -1; // no screen files on disk: the built-in screens are used
		}
		else if (stub == STUB_close || stub == STUB_fflush) {
			result = 0;
		}
		else if (stub == STUB_stack_chk_fail) {
			throw new IllegalStateException("stack check failed");
		}
		else {
			throw new IllegalStateException("no hook for " + Long.toHexString(stub));
		}

		// return to the caller
		long rsp = reg("RSP");
		long ret = readQ(rsp);
		emu.writeRegister("RSP", rsp + 8);
		emu.writeRegister("RAX", result);
		return ret;
	}

	/** Minimal printf: %s %c %d %i %u %ld %lu with optional zero padding and width. */
	private byte[] format(long fmtAddr, long[] regArgs) throws Exception {
		byte[] fmt = cbytes(fmtAddr);
		ByteArrayOutputStream out = new ByteArrayOutputStream();
		int argIndex = 0;
		for (int i = 0; i < fmt.length; i++) {
			if (fmt[i] != '%') {
				out.write(fmt[i]);
				continue;
			}
			i++;
			if (fmt[i] == '%') {
				out.write('%');
				continue;
			}
			boolean zero = false;
			int width = 0;
			if (fmt[i] == '0') {
				zero = true;
				i++;
			}
			while (fmt[i] >= '0' && fmt[i] <= '9') {
				width = width * 10 + (fmt[i] - '0');
				i++;
			}
			boolean wide = false;
			while (fmt[i] == 'l') {
				wide = true;
				i++;
			}
			long arg;
			if (argIndex < regArgs.length) {
				arg = regArgs[argIndex];
			}
			else { // on the stack, above the return address
				arg = readQ(reg("RSP") + 8 + 8L * (argIndex - regArgs.length));
			}
			argIndex++;

			byte[] piece;
			switch (fmt[i]) {
				case 's':
					piece = cbytes(arg);
					break;
				case 'c':
					piece = new byte[] { (byte) arg };
					break;
				case 'd':
				case 'i':
					piece = Long.toString(wide ? arg : (long) (int) arg)
							.getBytes(StandardCharsets.US_ASCII);
					break;
				case 'u':
					piece = (wide ? Long.toUnsignedString(arg) : Long.toString(arg & 0xffffffffL))
							.getBytes(StandardCharsets.US_ASCII);
					break;
				default:
					throw new IllegalStateException("unsupported printf conversion %" +
						(char) fmt[i] + " in " + new String(fmt, StandardCharsets.ISO_8859_1));
			}
			for (int pad = piece.length; pad < width; pad++) {
				out.write(zero ? '0' : ' ');
			}
			out.write(piece);
		}
		return out.toByteArray();
	}

	/** The bytes of a NUL-terminated string in emulator memory. */
	private byte[] cbytes(long addr) throws Exception {
		ByteArrayOutputStream out = new ByteArrayOutputStream();
		while (true) {
			byte[] chunk = emu.readMemory(toAddr(addr), 64);
			for (byte b : chunk) {
				if (b == 0) {
					return out.toByteArray();
				}
				out.write(b);
			}
			addr += chunk.length;
		}
	}

	private byte[] cstr(String text) {
		byte[] b = text.getBytes(StandardCharsets.ISO_8859_1);
		return java.util.Arrays.copyOf(b, b.length + 1);
	}

	private long string(String text) throws Exception {
		byte[] b = cstr(text);
		long p = alloc(b.length + 96); // room for in-place truncation and padding reads
		write(p, b);
		return p;
	}

	private long alloc(long size) throws Exception {
		long p = heap;
		long rounded = (size + 0x1f) & ~0xfL;
		fill(p, (byte) 0, rounded);
		heap += rounded + 0x100; // red zone between blocks
		return p;
	}

	private void fill(long addr, byte value, long len) throws Exception {
		if (len <= 0) {
			return;
		}
		byte[] b = new byte[(int) len];
		if (value != 0) {
			java.util.Arrays.fill(b, value);
		}
		emu.writeMemory(toAddr(addr), b);
	}

	private void write(long addr, byte[] b) throws Exception {
		emu.writeMemory(toAddr(addr), b);
	}

	private long reg(String name) {
		BigInteger v = emu.readRegister(name);
		return v.longValue();
	}

	private void writeQ(long addr, long v) throws Exception {
		byte[] b = new byte[8];
		for (int i = 0; i < 8; i++) {
			b[i] = (byte) (v >>> (8 * i));
		}
		emu.writeMemory(toAddr(addr), b);
	}

	private void writeD(long addr, int v) throws Exception {
		byte[] b = { (byte) v, (byte) (v >>> 8), (byte) (v >>> 16), (byte) (v >>> 24) };
		emu.writeMemory(toAddr(addr), b);
	}

	private void writeW(long addr, int v) throws Exception {
		byte[] b = { (byte) v, (byte) (v >>> 8) };
		emu.writeMemory(toAddr(addr), b);
	}

	private long readQ(long addr) throws Exception {
		byte[] b = emu.readMemory(toAddr(addr), 8);
		long v = 0;
		for (int i = 7; i >= 0; i--) {
			v = (v << 8) | (b[i] & 0xffL);
		}
		return v;
	}

	private int readW(long addr) throws Exception {
		byte[] b = emu.readMemory(toAddr(addr), 2);
		return (b[0] & 0xff) | ((b[1] & 0xff) << 8);
	}
}
