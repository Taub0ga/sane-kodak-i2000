/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Thomas Parthe */
/*
 * i2600-scand -- press the green button, get a PDF.
 *
 * The scanner waits in its energy saver showing a text of our choosing.  A
 * press of the big green key arrives as interrupt event 0x20 with id=1 and
 * does not wake the device by itself, so the daemon wakes it, pulls the
 * feeder empty, writes one PDF per batch into a watched folder -- a
 * paperless-ngx consume directory -- and puts the scanner back to sleep.
 *
 * Why this and not scanbd: scanbd does keep the device open while it polls,
 * which is what arms the event reporting, but when a sensor triggers it runs
 * sane_close(), the action script, and then sane_open() again (sane.c in
 * scanbd 1.5.1, lines 1024 and 1158).  That reopen runs our 271-transfer
 * open sequence, which wakes the scanner, and there is no hook afterwards to
 * send it back to sleep -- so the device would stand awake between scans.
 * It also has nowhere to put the panel text.  See NATIVES-BACKEND.md
 * section 16.
 *
 * The SANE backend keeps the --scan sensor for frontends that want it; this
 * is the unattended path.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i2600-core.h"
#include "i2600-firmware.h"
#include "i2600-pdf.h"
#include "panel-bitmaps.h"

#define EV_SCREEN_MISSING  0x60
#define POLL_MS            200

/* EX_TEMPFAIL: the scanner went away, which is not this daemon failing. */
#define I2600_EXIT_DEVICE_GONE 75

static volatile sig_atomic_t stop_requested;

static const char *out_dir;
static const char *prefix     = "i2600";
static int   want_crop        = 1;
static int   want_rotate      = 1;
static int   jpeg_quality     = 85;
static int   button_id        = 1;      /* the big green key, section 13 */
static int   use_saver        = 1;
static int   use_panel        = 1;
static int   drop_blank       = 1;      /* the back of a one-sided sheet */
static double blank_ink       = 0.05;   /* per cent of pixels, see below */
static int   once             = 0;
static int   verbose          = 0;
static unsigned char panel_ready[I2600_LCD_BYTES];
static unsigned screens_seen;

static void on_signal(int sig) { (void)sig; stop_requested = 1; }

static void say(const char *fmt, ...)
{
	char stamp[32];
	time_t now = time(NULL);
	struct tm tm;
	va_list ap;

	localtime_r(&now, &tm);
	strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
	fprintf(stderr, "[%s] ", stamp);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

/* Core progress messages, only when asked for -- they are chatty. */
static void core_log(const char *line)
{
	if (verbose)
		say("  %s", line);
}

static void msleep(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------- the panel */

/*
 * Store our waiting text where the device draws it while it sleeps: region 2
 * screen 3, the vendor's "Power saver mode".  An upload only fills the
 * screen behind the panel, the device decides when to draw it (section 14),
 * and the vendor commits an upload by writing the NvRam calibration block
 * back unchanged -- never anything invented, only the bytes just read.
 */
static void panel_store(void)
{
	unsigned char nv[I2600_NVRAM_BYTES];

	if (!use_panel)
		return;
	if (i2600_lcd_write(panel_ready, I2600_PANEL_READY_SCREEN,
			    I2600_PANEL_READY_REGION) < 0) {
		say("panel: upload of the waiting screen failed");
		return;
	}
	if (i2600_lcd_write(i2600_panel_scanning, I2600_PANEL_SCANNING_SCREEN,
			    I2600_PANEL_SCANNING_REGION) < 0)
		say("panel: upload of the scanning screen failed");
	if (i2600_nvram_read(nv) == I2600_NVRAM_BYTES)
		i2600_nvram_write(nv);
}

static int load_panel_file(const char *path)
{
	FILE *fh = fopen(path, "rb");
	size_t n;

	if (!fh) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}
	n = fread(panel_ready, 1, sizeof panel_ready, fh);
	fclose(fh);
	if (n != sizeof panel_ready) {
		fprintf(stderr, "%s: %zu bytes, expected %d\n",
			path, n, I2600_LCD_BYTES);
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------- output */

/*
 * Is this page empty enough to throw away?
 *
 * The back of a one-sided sheet is the whole reason: in duplex every second
 * page is blank.  paperless-ngx cannot do this itself -- the standing answer
 * there is a pre-consume script that measures ink with Ghostscript and cuts
 * pages with qpdf -- and here it is both easier and better, because the raw
 * pixels are still around, before JPEG, and a dropped page costs nothing
 * downstream.
 *
 * The measure is the share of pixels clearly darker than the sheet's own
 * white.  Two guards keep it from eating real pages:
 *
 *  - the white point is the *mode* of the bright half of the histogram, not
 *    an absolute value, so a grey or yellowed sheet is judged against itself;
 *  - a page whose mean is dark is never blank, whatever its ink share, which
 *    is what stops a photo from being mistaken for an empty sheet;
 *  - the outer one per cent of the sheet is left out of the measurement.
 *
 * That last one is not cosmetic, it is the whole thing.  Measured over a
 * real colour duplex batch, a blank back came out at 0.344 % inked and
 * barely moved when the cut was deepened -- those were genuinely dark
 * pixels, namely the two-pixel remnant of the transport border that the
 * autocrop leaves behind.  Ignoring a one per cent margin takes the same
 * page to 0.0006 %, while the thinnest page with real content stayed at
 * 6.7 %: a factor of ten thousand instead of a factor of twenty.
 *
 * The default threshold is deliberately timid on top of that.  One line of
 * 12 pt text at 300 dpi is about 0.17 % of an A4 page, and four punched
 * holes about the same, so 0.05 % keeps both.  Losing a real page is far
 * worse than keeping an empty one.
 */
static double page_ink_percent(const unsigned char *img, unsigned w,
			       unsigned lines, unsigned bpp)
{
	unsigned long hist[256] = { 0 };
	unsigned long sum = 0, ink = 0, total;
	unsigned mx = w / 100, my = lines / 100;     /* the margin to ignore */
	unsigned best = 0, white;
	unsigned long best_count = 0;
	unsigned x, y;
	size_t i;
	int cut;

	if (w <= 2 * mx || lines <= 2 * my)
		return 100.0;
	total = (unsigned long)(w - 2 * mx) * (lines - 2 * my);

	for (y = my; y < lines - my; y++) {
		const unsigned char *row = img + (size_t)y * w * bpp;

		for (x = mx; x < w - mx; x++) {
			const unsigned char *px = row + (size_t)x * bpp;
			/* Rec. 601 luma, integer: 0.299 / 0.587 / 0.114. */
			unsigned lum = bpp == 3
				? (77u * px[0] + 151u * px[1] + 28u * px[2]) >> 8
				: px[0];
			hist[lum]++;
			sum += lum;
		}
	}

	/*
	 * A page this dark has content, whatever the ink share says.
	 *
	 * The bound used to be 200, which worked only as long as the images
	 * came in uncorrected and clipped: a white sheet read 255 everywhere.
	 * Since the shading calibration is loaded (NATIVES-BACKEND.md section
	 * 17) paper white sits around 185, and a cropped blank sheet measures
	 * a mean of 183 -- under the old bound, so every blank page was
	 * reported as 100 % inked and none was ever dropped.  Measured on a
	 * cropped blank sheet the share is 0.0012 % at this bound, forty times
	 * under the 0.05 % limit, while the thinnest real page in the archive
	 * still comes out at 7.3 %.  Losing a real page is far worse than
	 * keeping an empty one, so the bound stays well below paper white.
	 */
	if (sum / total < 150)
		return 100.0;

	/* The sheet's own white: the most common bright value. */
	for (i = 128; i < 256; i++) {
		if (hist[i] > best_count) {
			best_count = hist[i];
			best = (unsigned)i;
		}
	}
	if (!best_count)
		return 100.0;
	white = best;

	cut = (int)white - 48;
	if (cut < 16)
		cut = 16;
	for (i = 0; i < (size_t)cut; i++)
		ink += hist[i];

	return 100.0 * (double)ink / (double)total;
}

/*
 * Hand one image block to the PDF, cropped and turned the right way up.
 * Returns 0 when the page went in, 1 when it was blank and dropped on
 * purpose, -1 when something was wrong with it.
 */
static int add_block(struct i2600_pdf *pdf, struct block *b)
{
	struct footer f = i2600_parse_footer(b->data, b->len);
	size_t image_bytes;
	unsigned width, lines, bpp;

	if (!f.ok) {
		say("block of %zu bytes has no readable footer -- dropped", b->len);
		return -1;
	}
	image_bytes = b->len - f.footer_bytes;
	width = f.width_pixels ? f.width_pixels : I2600_LINE_BYTES;
	bpp = i2600_bytes_per_pixel();
	lines = (unsigned)(image_bytes / ((size_t)width * bpp));
	if (!lines) {
		say("block carries no image lines -- dropped");
		return -1;
	}
	if (want_crop) {
		struct i2600_crop c;
		if (i2600_autocrop(b->data, width, lines, bpp, &c) == 0)
			lines = i2600_apply_crop(b->data, width, bpp, &c, &width);
	}
	if (want_rotate)
		i2600_rotate180(b->data, (size_t)width * lines * bpp, bpp);
	if (drop_blank) {
		double ink = page_ink_percent(b->data, width, lines, bpp);

		if (ink <= blank_ink) {
			say("blank page dropped (%.3f %% ink, limit %.3f %%)",
			    ink, blank_ink);
			return 1;              /* dropped, not failed */
		}
		if (verbose)
			say("  ink %.3f %%", ink);
	}
	if (i2600_pdf_add_page(pdf, b->data, width, lines, bpp,
			       I2600_DPI, jpeg_quality) < 0) {
		say("could not add a %u x %u page to the PDF", width, lines);
		return -1;
	}
	if (verbose)
		say("  page %u x %u pixels, %u byte/pixel", width, lines, bpp);
	if (lines < 200)
		say("WARNING: a page of only %u lines -- the scanner was not "
		    "prepared, or the sheet is tiny", lines);
	return 0;
}

/*
 * paperless watches its consume directory and picks up whatever lands there,
 * so a half-written file must never carry the final name.  Write to
 * NAME.pdf.part -- an extension paperless does not consume -- and rename
 * when the batch is complete; rename is atomic within one directory.
 */
static void batch_names(char *final, size_t fn, char *part, size_t pn)
{
	char stamp[32];
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
	snprintf(final, fn, "%s/%s-%s.pdf", out_dir, prefix, stamp);
	snprintf(part, pn, "%s.part", final);
}

/*
 * Park the scanner: lamp out, then the energy saver.
 *
 * Neither the saver nor the vendor's EnterEnergyStar() switches the lamp
 * off -- it only runs SetEnergyStar(1) and ScannerOperation 0x16 -- so a
 * device left waiting glows all day unless it is told.  Two details are
 * measured and both matter (section 16): the lamp only listens while the
 * device is awake, and it ignores an off sent in the same breath as the
 * prepare phase switched it on, because the firmware is still running its
 * warm-up (hence GetLampWarmupTime and IsOkToStartScan(ELAMP_WARMUP_TYPE)).
 * So: wait out the warm-up, ask again, and only then sleep.
 */
static void go_to_sleep(void)
{
	i2600_lamp(0);
	msleep(1500);
	if (i2600_lamp(0) < 0)
		say("could not switch the lamp off");
	if (!use_saver)
		return;
	say("back to sleep");
	if (i2600_power_saver(1) < 0)
		say("could not enter the energy saver");
}

/* ------------------------------------------------------------ one batch */

static int run_batch(void)
{
	struct i2600_pdf pdf;
	char final[512], part[600];
	int pages = 0, bad = 0, blanks = 0;
	enum i2600_ready ready;

	batch_names(final, sizeof final, part, sizeof part);

	if (use_saver) {
		say("waking the scanner");
		if (i2600_power_saver(0) < 0) {
			say("could not leave the energy saver");
			return -1;
		}
		/* Wait for the device to report itself awake instead of
		 * guessing a delay; the sensors read nonsense until then. */
		for (int i = 0; i < 40; i++) {
			int st = i2600_power_state();
			if (st >= 0 && st != I2600_POWER_SAVER)
				break;
			msleep(100);
		}
	}

	/*
	 * The energy saver costs the device its camera and lamp setup, so the
	 * prepare phase has to run again -- without it the transport pulls the
	 * sheets through and hands back 76 black lines per page.  Cheap enough
	 * to do unconditionally: 171 control transfers, about a second.
	 */
	if (i2600_device_prepare() < 0) {
		say("could not prepare the scanner after waking");
		return -1;
	}

	ready = i2600_check_ready();
	if (ready == I2600_READY_NO_DOCS) {
		say("no paper in the feeder -- nothing to do");
		return 0;
	}
	if (ready != I2600_READY_OK) {
		static const char *why[] = {
			"ready", "firmware not running", "cover open",
			"paper jam", "feeder empty", "transport busy",
			"critical error", "cannot talk to the device"
		};
		say("not ready to scan: %s", why[ready]);
		return -1;
	}

	if (i2600_pdf_begin(&pdf, part) < 0) {
		say("cannot write %s: %s", part, strerror(errno));
		return -1;
	}
	if (i2600_scan_begin() < 0) {
		say("the scan sequence failed to start");
		i2600_pdf_abort(&pdf);
		return -1;
	}
	for (;;) {
		struct block f = { NULL, 0 }, r = { NULL, 0 };
		int got = i2600_scan_next_page(&f, i2600_duplex ? &r : NULL);
		int side;

		if (got <= 0)
			break;
		for (side = 0; side < 2; side++) {
			struct block *b = side ? &r : &f;
			int rc;

			if (side && !b->data)
				break;
			rc = add_block(&pdf, b);
			if (rc == 0)      pages++;
			else if (rc > 0)  blanks++;
			else              bad++;
			i2600_block_free(b);
		}
		if (pages >= I2600_PDF_MAX_PAGES) {
			say("page limit of %d reached -- closing this batch",
			    I2600_PDF_MAX_PAGES);
			break;
		}
		if (i2600_device_gone()) {
			i2600_pdf_abort(&pdf);
			say("the scanner disappeared mid-batch -- %s dropped", part);
			return -1;
		}
	}
	i2600_scan_end(0);

	switch (i2600_scan_stop_reason()) {
	case I2600_STOP_PAPER_JAM:
		say("PAPER JAM -- clear the paper path by hand; this scanner "
		    "usually needs its power switch");
		break;
	case I2600_STOP_STALLED:
		/* The feeder sensor often still reads "paper" right after the
		 * last sheet, so this is not reliably a short batch -- say what
		 * was measured, not what it might mean. */
		say("the transport stopped while the feeder sensor still read "
		    "paper; press again if a sheet was left behind");
		break;
	default:
		break;
	}

	if (!pages) {
		i2600_pdf_abort(&pdf);
		if (blanks)
			say("every one of the %d page(s) read as blank -- "
			    "nothing written; use --blank-ink or --keep-blank "
			    "if that is wrong", blanks);
		else
			say("no usable page in this batch, nothing written");
		return -1;
	}
	if (i2600_pdf_end(&pdf) < 0) {
		say("writing %s failed", part);
		return -1;
	}
	if (rename(part, final) < 0) {
		say("cannot rename %s: %s", part, strerror(errno));
		unlink(part);
		return -1;
	}
	say("%s -- %d page(s)%s%s", final, pages,
	    blanks ? " (blank backs skipped)" : "",
	    bad ? " (some blocks were unreadable)" : "");
	return pages;
}

/* ----------------------------------------------------------------- main */

static void usage(const char *argv0)
{
	fprintf(stderr,
	"usage: %s [options]\n"
	"  The scanner sleeps showing its panel text; a press of the green\n"
	"  button scans the feeder into one PDF and it goes back to sleep.\n"
	"\n"
	"  -o, --out DIR       where the PDFs go, normally paperless' consume\n"
	"                      directory (default: $I2600_CONSUME_DIR, else\n"
	"                      $HOME/paperless-consume)\n"
	"  -n, --prefix NAME   file name prefix (default: i2600)\n"
	"  -d, --duplex        scan both sides of every sheet\n"
	"  -c, --color         24 bit colour instead of 8 bit grey\n"
	"  -q, --quality N     JPEG quality inside the PDF, 1..100 (default 85)\n"
	"  -b, --button ID     which key triggers: 1 is the green one, 0 and 3\n"
	"                      are the arrows, -1 means any (default 1)\n"
	"  -1, --once          scan one batch, then exit\n"
	"      --no-crop       keep the dark transport border\n"
	"      --no-rotate     do not turn the pages upside down\n"
	"      --no-saver      leave the scanner awake between batches\n"
	"      --no-panel      do not touch the panel text\n"
	"      --panel FILE    768-byte bitmap for the waiting screen,\n"
	"                      instead of the built-in \"Bereit\"\n"
	"                      (make one with tools/lcd-text.py)\n"
	"      --keep-blank    keep empty pages -- by default the blank back\n"
	"                      of a one-sided sheet is left out\n"
	"      --blank-ink P   a page counts as blank below P per cent inked\n"
	"                      pixels (default 0.05; one line of text is\n"
	"                      about 0.17, so this keeps it)\n"
	"  -F, --firmware SRC  where to take the firmware images from\n"
	"                      (default: %s)\n"
	"  -v, --verbose       log every step\n",
	argv0, I2600_FW_DEFAULT_SOURCE);
}

static int ensure_out_dir(void)
{
	struct stat st;

	if (stat(out_dir, &st) == 0) {
		if (!S_ISDIR(st.st_mode)) {
			fprintf(stderr, "%s is not a directory\n", out_dir);
			return -1;
		}
	} else if (mkdir(out_dir, 0755) < 0) {
		fprintf(stderr, "cannot create %s: %s\n", out_dir, strerror(errno));
		return -1;
	}
	if (access(out_dir, W_OK) < 0) {
		fprintf(stderr, "cannot write to %s: %s\n", out_dir, strerror(errno));
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	const char *panel_file = NULL;
	int c;

	enum { OPT_NO_CROP = 1000, OPT_NO_ROTATE, OPT_NO_SAVER, OPT_NO_PANEL,
	       OPT_PANEL, OPT_KEEP_BLANK, OPT_BLANK_INK };
	static const struct option opts[] = {
		{ "out",       required_argument, NULL, 'o' },
		{ "prefix",    required_argument, NULL, 'n' },
		{ "duplex",    no_argument,       NULL, 'd' },
		{ "color",     no_argument,       NULL, 'c' },
		{ "quality",   required_argument, NULL, 'q' },
		{ "button",    required_argument, NULL, 'b' },
		{ "once",      no_argument,       NULL, '1' },
		{ "firmware",  required_argument, NULL, 'F' },
		{ "verbose",   no_argument,       NULL, 'v' },
		{ "help",      no_argument,       NULL, 'h' },
		{ "no-crop",   no_argument,       NULL, OPT_NO_CROP },
		{ "no-rotate", no_argument,       NULL, OPT_NO_ROTATE },
		{ "no-saver",  no_argument,       NULL, OPT_NO_SAVER },
		{ "no-panel",  no_argument,       NULL, OPT_NO_PANEL },
		{ "panel",     required_argument, NULL, OPT_PANEL },
		{ "keep-blank", no_argument,      NULL, OPT_KEEP_BLANK },
		{ "blank-ink", required_argument, NULL, OPT_BLANK_INK },
		{ NULL, 0, NULL, 0 }
	};

	memcpy(panel_ready, i2600_panel_ready, sizeof panel_ready);

	while ((c = getopt_long(argc, argv, "o:n:dcq:b:1F:vh", opts, NULL)) != -1) {
		switch (c) {
		case 'o': out_dir = optarg; break;
		case 'n': prefix = optarg; break;
		case 'd': i2600_duplex = 1; break;
		case 'c': i2600_color = 1; break;
		case 'q': jpeg_quality = atoi(optarg); break;
		case 'b': button_id = atoi(optarg); break;
		case '1': once = 1; break;
		case 'F': i2600_fw_source = optarg; break;
		case 'v': verbose = 1; break;
		case OPT_NO_CROP:   want_crop = 0; break;
		case OPT_NO_ROTATE: want_rotate = 0; break;
		case OPT_NO_SAVER:  use_saver = 0; break;
		case OPT_NO_PANEL:  use_panel = 0; break;
		case OPT_PANEL:     panel_file = optarg; break;
		case OPT_KEEP_BLANK: drop_blank = 0; break;
		case OPT_BLANK_INK: blank_ink = atof(optarg); break;
		default: usage(argv[0]); return c == 'h' ? 0 : 2;
		}
	}
	if (jpeg_quality < 1 || jpeg_quality > 100) {
		fprintf(stderr, "quality must be 1..100\n");
		return 2;
	}
	if (blank_ink < 0 || blank_ink > 100) {
		fprintf(stderr, "blank-ink must be a percentage 0..100\n");
		return 2;
	}
	if (panel_file && load_panel_file(panel_file) < 0)
		return 2;

	if (!out_dir)
		out_dir = getenv("I2600_CONSUME_DIR");
	if (!out_dir) {
		static char buf[512];
		const char *home = getenv("HOME");
		if (!home) {
			fprintf(stderr, "no HOME and no --out: nowhere to put the files\n");
			return 2;
		}
		snprintf(buf, sizeof buf, "%s/paperless-consume", home);
		out_dir = buf;
	}
	if (ensure_out_dir() < 0)
		return 1;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	i2600_log_sink = core_log;

	/*
	 * Sit and wait rather than exit: switching the scanner off is how a
	 * paper jam gets cleared here, and a daemon that only exits is back
	 * no sooner than RestartSec.  This returns the moment the device is
	 * on the bus and openable.
	 */
	if (i2600_wait_for_device(0) < 0) {
		say("gave up waiting for the scanner");
		return 1;
	}
	say("opening the scanner");
	if (i2600_device_open() < 0) {
		say("the scanner is there but would not open -- is another "
		    "program holding it?");
		return 1;
	}
	say("ready -- files go to %s", out_dir);

	panel_store();
	screens_seen = i2600_event_count(EV_SCREEN_MISSING);
	i2600_flush_buttons();
	go_to_sleep();

	while (!stop_requested) {
		uint8_t id;
		uint16_t screen;
		unsigned now;

		if (i2600_device_gone())
			break;

		if (i2600_next_button(&id, &screen)) {
			if (button_id >= 0 && id != (uint8_t)button_id) {
				if (verbose)
					say("button id=%u ignored", id);
				continue;
			}
			say("button id=%u -- scanning", id);
			if (run_batch() < 0 && i2600_device_gone())
				continue;   /* handled at the top of the loop */
			/* Presses during a run are the user being impatient,
			 * not a second batch. */
			i2600_flush_buttons();
			panel_store();
			go_to_sleep();
			if (once)
				break;
			continue;
		}

		/*
		 * A Screen Missing event is the device asking for a bitmap it
		 * does not have (section 14), so refill the screens when one
		 * turns up -- that is the only moment an upload is invited.
		 */
		now = i2600_event_count(EV_SCREEN_MISSING);
		if (now != screens_seen) {
			screens_seen = now;
			if (verbose)
				say("device asked for a panel screen");
			panel_store();
		}
		msleep(POLL_MS);
	}

	/*
	 * A scanner switched off cannot be talked to, and switching it on
	 * again enumerates a new device this process cannot reach.  So leave,
	 * without the teardown sequence -- it would only pile up failed
	 * transfers -- and let systemd start us again (Restart=always).  The
	 * fresh process then finds a cold scanner and downloads its firmware
	 * like any other cold start, which is what makes headless operation
	 * survive the power switch.
	 */
	if (i2600_device_gone()) {
		say("the scanner was switched off -- exiting so the service "
		    "starts over and waits for it");
		/* Its own exit code, so the unit can call this one a normal
		 * end (SuccessExitStatus) and the journal does not read like
		 * a fault every time someone clears a jam at the switch. */
		return I2600_EXIT_DEVICE_GONE;
	}

	say("shutting down");
	/* The teardown sequence wakes the device; leaving it asleep would mean
	 * leaving the interface claimed, which is worse. */
	i2600_device_close();
	return 0;
}
