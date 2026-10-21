/*
 * libsane-i2600 -- a SANE backend for the Kodak i2600, on top of i2600-core.
 *
 * The device has exactly one mode: 2580 pixels per line, 8 bit grey, 300 dpi,
 * already cropped by its FPGA (see NATIVES-BACKEND.md).  Resolution, bit depth
 * and bitonal conversion are the frontend's job, so this backend offers a
 * deliberately small option set and hands over raw grey lines.
 *
 * Duplex follows the usual ADF convention: every sane_start() yields one
 * image, front then rear, and the rear side is rotated 180 degrees because
 * the second camera sees the sheet from the other side.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sane/sane.h>
#include <sane/saneopts.h>

#include "i2600-core.h"

#define I2600_SANE_VERSION  SANE_VERSION_CODE(1, 0, 0)
/* 2580 px at 300 dpi = 218.44 mm; the length is whatever the sheet turns
 * out to be, so the height range just has to be generous. */
#define WIDTH_MM            218.44
#define MAX_LENGTH_MM       900.0

enum {
	OPT_NUM_OPTIONS = 0,
	OPT_MODE,
	OPT_RESOLUTION,
	OPT_SOURCE,
	OPT_CROP,
	OPT_ROTATE,
	OPT_TL_X, OPT_TL_Y, OPT_BR_X, OPT_BR_Y,
	OPT_SENSOR_BUTTON,
	OPT_FUNCTION,
	NUM_OPTIONS
};

static const SANE_String_Const mode_list[]   = { SANE_VALUE_SCAN_MODE_GRAY,
						 SANE_VALUE_SCAN_MODE_COLOR, NULL };
static const SANE_String_Const source_list[] = { "ADF Front", "ADF Duplex", NULL };
static const SANE_Word         dpi_list[]    = { 1, I2600_DPI };

static SANE_Option_Descriptor opt[NUM_OPTIONS];
static SANE_Range x_range, y_range;

static int  opened;
static int  dev_up;        /* USB side actually brought up */
static int  want_duplex;
static int  batch_running;
static struct block cur, pending_rear;
static size_t cur_off, cur_image_bytes;
static unsigned cur_width, cur_lines;
static int  cur_drained;   /* current image was read to EOF */
static char source_value[32] = "ADF Front";
static char mode_value[32]   = SANE_VALUE_SCAN_MODE_GRAY;
static int  want_color;
static SANE_Bool crop_value = SANE_TRUE;
static SANE_Bool rotate_value = SANE_TRUE;
static SANE_Word button_id;     /* which key the last reported press came from */

static const SANE_Device dev_desc = {
	"i2600", "Kodak", "i2600", "sheetfed scanner"
};
static const SANE_Device *dev_list[] = { &dev_desc, NULL };

/* ------------------------------------------------------------- options */

static void init_options(void)
{
	int i;
	memset(opt, 0, sizeof opt);
	x_range.min = SANE_FIX(0.0); x_range.max = SANE_FIX(WIDTH_MM);     x_range.quant = 0;
	y_range.min = SANE_FIX(0.0); y_range.max = SANE_FIX(MAX_LENGTH_MM); y_range.quant = 0;

	opt[OPT_NUM_OPTIONS].name  = SANE_NAME_NUM_OPTIONS;
	opt[OPT_NUM_OPTIONS].title = SANE_TITLE_NUM_OPTIONS;
	opt[OPT_NUM_OPTIONS].desc  = SANE_DESC_NUM_OPTIONS;
	opt[OPT_NUM_OPTIONS].type  = SANE_TYPE_INT;
	opt[OPT_NUM_OPTIONS].size  = sizeof(SANE_Word);
	opt[OPT_NUM_OPTIONS].cap   = SANE_CAP_SOFT_DETECT;

	opt[OPT_MODE].name  = SANE_NAME_SCAN_MODE;
	opt[OPT_MODE].title = SANE_TITLE_SCAN_MODE;
	opt[OPT_MODE].desc  = "8 bit grey, or 24 bit colour (3 bytes per pixel "
			      "on the wire).";
	opt[OPT_MODE].type  = SANE_TYPE_STRING;
	opt[OPT_MODE].size  = 32;
	opt[OPT_MODE].constraint_type = SANE_CONSTRAINT_STRING_LIST;
	opt[OPT_MODE].constraint.string_list = mode_list;
	opt[OPT_MODE].cap = SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT;

	opt[OPT_RESOLUTION].name  = SANE_NAME_SCAN_RESOLUTION;
	opt[OPT_RESOLUTION].title = SANE_TITLE_SCAN_RESOLUTION;
	opt[OPT_RESOLUTION].desc  = "The device always scans at 300 dpi.";
	opt[OPT_RESOLUTION].type  = SANE_TYPE_INT;
	opt[OPT_RESOLUTION].unit  = SANE_UNIT_DPI;
	opt[OPT_RESOLUTION].size  = sizeof(SANE_Word);
	opt[OPT_RESOLUTION].constraint_type = SANE_CONSTRAINT_WORD_LIST;
	opt[OPT_RESOLUTION].constraint.word_list = dpi_list;
	opt[OPT_RESOLUTION].cap = SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT;

	opt[OPT_SOURCE].name  = SANE_NAME_SCAN_SOURCE;
	opt[OPT_SOURCE].title = SANE_TITLE_SCAN_SOURCE;
	opt[OPT_SOURCE].desc  = "Single sided or both sides of the sheet.";
	opt[OPT_SOURCE].type  = SANE_TYPE_STRING;
	opt[OPT_SOURCE].size  = sizeof source_value;
	opt[OPT_SOURCE].constraint_type = SANE_CONSTRAINT_STRING_LIST;
	opt[OPT_SOURCE].constraint.string_list = source_list;
	opt[OPT_SOURCE].cap = SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT;

	opt[OPT_CROP].name  = "crop";
	opt[OPT_CROP].title = "Crop to the sheet";
	opt[OPT_CROP].desc  = "Trim the dark transport border around the page. "
			      "The device always sends the whole transport window; "
			      "this finds the sheet inside it.";
	opt[OPT_CROP].type  = SANE_TYPE_BOOL;
	opt[OPT_CROP].size  = sizeof(SANE_Word);
	opt[OPT_CROP].cap   = SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT;

	opt[OPT_ROTATE].name  = "rotate";
	opt[OPT_ROTATE].title = "Rotate by 180 degrees";
	opt[OPT_ROTATE].desc  = "Turn every page upside down.  Both cameras hand "
				"the sheet over the same way up, which comes out "
				"inverted when sheets are loaded face up -- the "
				"natural way round for this feeder.";
	opt[OPT_ROTATE].type  = SANE_TYPE_BOOL;
	opt[OPT_ROTATE].size  = sizeof(SANE_Word);
	opt[OPT_ROTATE].cap   = SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT;

	for (i = OPT_TL_X; i <= OPT_BR_Y; i++) {
		opt[i].type = SANE_TYPE_FIXED;
		opt[i].unit = SANE_UNIT_MM;
		opt[i].size = sizeof(SANE_Word);
		opt[i].constraint_type = SANE_CONSTRAINT_RANGE;
		/* The FPGA crops and the sheet decides the length, so these are
		 * reported rather than obeyed -- but they must be settable, or
		 * frontends that set a scan area (scanimage --batch, simple-scan)
		 * fail outright. */
		opt[i].cap  = SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT;
		opt[i].constraint.range = (i == OPT_TL_X || i == OPT_BR_X)
					? &x_range : &y_range;
	}
	/* The panel's start button, as a SANE sensor: scanbd and friends poll
	 * this and start a scan when it reads true.  SANE_CAP_HARD_SELECT means
	 * "the user operates this on the device, not in software". */
	opt[OPT_SENSOR_BUTTON].name  = SANE_NAME_SCAN;
	opt[OPT_SENSOR_BUTTON].title = "Scan button";
	opt[OPT_SENSOR_BUTTON].desc  = "True once after the start button on the "
				       "scanner was pressed.  Reading it clears it.";
	opt[OPT_SENSOR_BUTTON].type  = SANE_TYPE_BOOL;
	opt[OPT_SENSOR_BUTTON].size  = sizeof(SANE_Word);
	opt[OPT_SENSOR_BUTTON].unit  = SANE_UNIT_NONE;
	opt[OPT_SENSOR_BUTTON].cap   = SANE_CAP_SOFT_DETECT | SANE_CAP_HARD_SELECT
				     | SANE_CAP_ADVANCED;

	/* Which key was pressed.  The event's `id` field tells them apart --
	 * 1 is the big green start button, 0 and 3 are the arrows.  value1,
	 * which this used to report, is only the screen number and is the same
	 * for all three (NATIVES-BACKEND.md section 13). */
	opt[OPT_FUNCTION].name  = "function";
	opt[OPT_FUNCTION].title = "Panel function number";
	opt[OPT_FUNCTION].desc  = "Which key the last press came from: 1 is the "
				  "big green start button, 0 and 3 are the arrows.";
	opt[OPT_FUNCTION].type  = SANE_TYPE_INT;
	opt[OPT_FUNCTION].size  = sizeof(SANE_Word);
	opt[OPT_FUNCTION].cap   = SANE_CAP_SOFT_DETECT | SANE_CAP_ADVANCED;

	opt[OPT_TL_X].name = SANE_NAME_SCAN_TL_X; opt[OPT_TL_X].title = SANE_TITLE_SCAN_TL_X;
	opt[OPT_TL_Y].name = SANE_NAME_SCAN_TL_Y; opt[OPT_TL_Y].title = SANE_TITLE_SCAN_TL_Y;
	opt[OPT_BR_X].name = SANE_NAME_SCAN_BR_X; opt[OPT_BR_X].title = SANE_TITLE_SCAN_BR_X;
	opt[OPT_BR_Y].name = SANE_NAME_SCAN_BR_Y; opt[OPT_BR_Y].title = SANE_TITLE_SCAN_BR_Y;
	opt[OPT_TL_X].desc = "Left edge."; opt[OPT_TL_Y].desc = "Top edge.";
	opt[OPT_BR_X].desc = "Right edge."; opt[OPT_BR_Y].desc = "Bottom edge.";
}

/* ---------------------------------------------------------- SANE entry */

SANE_Status sane_init(SANE_Int *version_code, SANE_Auth_Callback authorize)
{
	(void)authorize;
	if (version_code)
		*version_code = I2600_SANE_VERSION;
	init_options();
	return SANE_STATUS_GOOD;
}

void sane_exit(void) { }

SANE_Status sane_get_devices(const SANE_Device ***list, SANE_Bool local_only)
{
	(void)local_only;
	*list = dev_list;
	return SANE_STATUS_GOOD;
}

SANE_Status sane_open(SANE_String_Const name, SANE_Handle *h)
{
	if (name && *name && strcmp(name, "i2600") != 0)
		return SANE_STATUS_INVAL;
	if (opened)
		return SANE_STATUS_DEVICE_BUSY;
	/* The USB side is brought up on the first sane_start(), not here:
	 * frontends set options after opening, and the duplex choice decides
	 * which vendor sequence runs.  It also keeps `scanimage -L` from
	 * downloading firmware into a cold scanner. */
	opened = 1;
	*h = (SANE_Handle)&dev_desc;
	return SANE_STATUS_GOOD;
}

static int bring_device_up(void)
{
	if (dev_up)
		return 0;
	i2600_verbose = 0;              /* a backend must not write to stdout */
	i2600_duplex  = want_duplex;
	i2600_color   = want_color;
	if (i2600_device_open() < 0)
		return -1;
	dev_up = 1;
	/* Presses from before the frontend was listening are not news. */
	i2600_flush_buttons();
	return 0;
}

static void drop_current(void)
{
	i2600_block_free(&cur);
	cur_off = cur_image_bytes = 0;
	cur_width = cur_lines = 0;
}

void sane_close(SANE_Handle h)
{
	(void)h;
	if (!opened)
		return;
	if (batch_running) {
		i2600_scan_end(1);
		batch_running = 0;
	}
	drop_current();
	i2600_block_free(&pending_rear);
	if (dev_up) {
		i2600_device_close();
		dev_up = 0;
	}
	opened = 0;
}

const SANE_Option_Descriptor *sane_get_option_descriptor(SANE_Handle h, SANE_Int n)
{
	(void)h;
	if (n < 0 || n >= NUM_OPTIONS)
		return NULL;
	return &opt[n];
}

SANE_Status sane_control_option(SANE_Handle h, SANE_Int n, SANE_Action a,
				void *v, SANE_Int *info)
{
	(void)h;
	if (n < 0 || n >= NUM_OPTIONS)
		return SANE_STATUS_INVAL;
	if (info)
		*info = 0;

	if (a == SANE_ACTION_GET_VALUE) {
		switch (n) {
		case OPT_NUM_OPTIONS: *(SANE_Word *)v = NUM_OPTIONS; return SANE_STATUS_GOOD;
		case OPT_MODE:        strcpy(v, mode_value); return SANE_STATUS_GOOD;
		case OPT_RESOLUTION:  *(SANE_Word *)v = I2600_DPI; return SANE_STATUS_GOOD;
		case OPT_SOURCE:      strcpy(v, source_value); return SANE_STATUS_GOOD;
		case OPT_CROP:        *(SANE_Word *)v = crop_value; return SANE_STATUS_GOOD;
		case OPT_ROTATE:      *(SANE_Word *)v = rotate_value; return SANE_STATUS_GOOD;
		case OPT_TL_X: case OPT_TL_Y: *(SANE_Word *)v = SANE_FIX(0.0); return SANE_STATUS_GOOD;
		case OPT_BR_X: *(SANE_Word *)v = SANE_FIX(WIDTH_MM); return SANE_STATUS_GOOD;
		case OPT_BR_Y: *(SANE_Word *)v = SANE_FIX(MAX_LENGTH_MM); return SANE_STATUS_GOOD;
		case OPT_SENSOR_BUTTON: {
			uint8_t id;
			/* Polling the button needs the device up; a frontend may
			 * ask long before it ever scans. */
			if (!dev_up && bring_device_up() < 0) {
				*(SANE_Word *)v = SANE_FALSE;
				return SANE_STATUS_GOOD;
			}
			/* Take presses off the queue so `function` can name the
			 * key this press came from, not the one before it.  A
			 * frontend that polls slower than the user types sees
			 * every press, one poll at a time. */
			if (i2600_next_button(&id, NULL)) {
				button_id = id;
				*(SANE_Word *)v = SANE_TRUE;
			} else {
				*(SANE_Word *)v = SANE_FALSE;
			}
			return SANE_STATUS_GOOD;
		}
		case OPT_FUNCTION:
			*(SANE_Word *)v = (SANE_Word)button_id;
			return SANE_STATUS_GOOD;
		default: return SANE_STATUS_INVAL;
		}
	}
	if (a == SANE_ACTION_SET_VALUE) {
		switch (n) {
		case OPT_MODE:
			if (strcmp(v, SANE_VALUE_SCAN_MODE_GRAY)
			    && strcmp(v, SANE_VALUE_SCAN_MODE_COLOR))
				return SANE_STATUS_INVAL;
			if (dev_up)     /* the sequence is fixed once it runs */
				return SANE_STATUS_INVAL;
			snprintf(mode_value, sizeof mode_value, "%s", (const char *)v);
			want_color = strcmp(mode_value, SANE_VALUE_SCAN_MODE_COLOR) == 0;
			if (info)
				*info = SANE_INFO_RELOAD_PARAMS;
			return SANE_STATUS_GOOD;
		case OPT_RESOLUTION:
			return *(SANE_Word *)v == I2600_DPI
			       ? SANE_STATUS_GOOD : SANE_STATUS_INVAL;
		case OPT_SOURCE:
			if (strcmp(v, "ADF Front") && strcmp(v, "ADF Duplex"))
				return SANE_STATUS_INVAL;
			if (dev_up)        /* the sequence is fixed once it runs */
				return SANE_STATUS_INVAL;
			snprintf(source_value, sizeof source_value, "%s", (const char *)v);
			want_duplex = strcmp(source_value, "ADF Duplex") == 0;
			if (info)
				*info = SANE_INFO_RELOAD_PARAMS;
			return SANE_STATUS_GOOD;
		case OPT_CROP:
			crop_value = *(SANE_Word *)v ? SANE_TRUE : SANE_FALSE;
			if (info)
				*info = SANE_INFO_RELOAD_PARAMS;
			return SANE_STATUS_GOOD;
		case OPT_ROTATE:
			rotate_value = *(SANE_Word *)v ? SANE_TRUE : SANE_FALSE;
			return SANE_STATUS_GOOD;
		case OPT_TL_X: case OPT_TL_Y: case OPT_BR_X: case OPT_BR_Y:
			/* Accepted and ignored: the device crops in hardware. */
			if (info)
				*info = SANE_INFO_INEXACT;
			return SANE_STATUS_GOOD;
		default:
			return SANE_STATUS_INVAL;
		}
	}
	return SANE_STATUS_UNSUPPORTED;
}

SANE_Status sane_get_parameters(SANE_Handle h, SANE_Parameters *p)
{
	(void)h;
	memset(p, 0, sizeof *p);
	p->format = want_color ? SANE_FRAME_RGB : SANE_FRAME_GRAY;
	p->last_frame = SANE_TRUE;
	p->depth = 8;
	p->pixels_per_line = cur_width ? (SANE_Int)cur_width : I2600_LINE_BYTES;
	p->bytes_per_line  = p->pixels_per_line * (want_color ? 3 : 1);
	/* The sheet decides the length, so before the first block the line
	 * count is genuinely unknown; SANE spells that -1. */
	p->lines = cur_lines ? (SANE_Int)cur_lines : -1;
	return SANE_STATUS_GOOD;
}

/* Strip the footer and make the block ready for sane_read(). */
static SANE_Status take_block(struct block *b, int rotate)
{
	struct footer f = i2600_parse_footer(b->data, b->len);
	if (!f.ok) {
		i2600_block_free(b);
		return SANE_STATUS_IO_ERROR;
	}
	cur = *b;
	b->data = NULL;
	b->len = 0;
	{
		unsigned bpp = i2600_bytes_per_pixel();
		cur_image_bytes = cur.len - f.footer_bytes;
		cur_width = f.width_pixels ? f.width_pixels : I2600_LINE_BYTES;
		cur_lines = (unsigned)(cur_image_bytes / ((size_t)cur_width * bpp));
		cur_image_bytes = (size_t)cur_lines * cur_width * bpp;
		cur_off = 0;
		(void)rotate;
		if (crop_value && cur_lines) {
			struct i2600_crop c;
			if (i2600_autocrop(cur.data, cur_width, cur_lines,
					   bpp, &c) == 0) {
				cur_lines = i2600_apply_crop(cur.data, cur_width,
							     bpp, &c, &cur_width);
				cur_image_bytes = (size_t)cur_width * cur_lines * bpp;
			}
			/* No page found: keep the full frame rather than guess. */
		}
		/* Applied to front and rear alike, after cropping: both cameras
		 * deliver the sheet the same way up. */
		if (rotate_value && cur_lines)
			i2600_rotate180(cur.data, cur_image_bytes, bpp);
	}
	return cur_lines ? SANE_STATUS_GOOD : SANE_STATUS_IO_ERROR;
}

SANE_Status sane_start(SANE_Handle h)
{
	struct block f = { NULL, 0 }, r = { NULL, 0 };
	int got;

	(void)h;
	if (!opened)
		return SANE_STATUS_INVAL;
	cur_drained = 0;
	if (!dev_up && bring_device_up() < 0)
		return SANE_STATUS_IO_ERROR;
	drop_current();

	/* A duplex sheet yields two images; serve the rear one first-come.
	 * No rotation: the vendor driver does not rotate either, and both
	 * cameras deliver the sheet the same way up -- see section 7. */
	if (pending_rear.data) {
		struct block tmp = pending_rear;
		pending_rear.data = NULL;
		pending_rear.len = 0;
		return take_block(&tmp, 0);
	}
	if (!batch_running) {
		/* Ask the device why it is not ready before trying, so the
		 * frontend shows "feeder empty" or "paper jam" instead of a
		 * generic failure after a pointless attempt. */
		switch (i2600_check_ready()) {
		case I2600_READY_OK:         break;
		case I2600_READY_NO_DOCS:    return SANE_STATUS_NO_DOCS;
		case I2600_READY_PAPER_JAM:  return SANE_STATUS_JAMMED;
		case I2600_READY_COVER_OPEN: return SANE_STATUS_COVER_OPEN;
		case I2600_READY_BUSY:       return SANE_STATUS_DEVICE_BUSY;
		default:                     return SANE_STATUS_IO_ERROR;
		}
		if (i2600_scan_begin() < 0)
			return SANE_STATUS_IO_ERROR;
		batch_running = 1;
	}
	got = i2600_scan_next_page(&f, i2600_duplex ? &r : NULL);
	if (got < 0)
		return SANE_STATUS_IO_ERROR;
	if (got == 0) {
		i2600_scan_end(0);
		batch_running = 0;
		switch (i2600_scan_stop_reason()) {
		case I2600_STOP_PAPER_JAM: return SANE_STATUS_JAMMED;
		case I2600_STOP_ERROR:     return SANE_STATUS_IO_ERROR;
		default:                   return SANE_STATUS_NO_DOCS;
		}
	}
	if (r.data)
		pending_rear = r;
	return take_block(&f, 0);
}

SANE_Status sane_read(SANE_Handle h, SANE_Byte *buf, SANE_Int maxlen, SANE_Int *len)
{
	size_t left;

	(void)h;
	*len = 0;
	if (!cur.data) {
		cur_drained = 1;
		return SANE_STATUS_EOF;
	}
	left = cur_image_bytes - cur_off;
	if (!left) {
		cur_drained = 1;
		drop_current();
		return SANE_STATUS_EOF;
	}
	if ((size_t)maxlen < left)
		left = (size_t)maxlen;
	memcpy(buf, cur.data + cur_off, left);
	cur_off += left;
	*len = (SANE_Int)left;
	return SANE_STATUS_GOOD;
}

/*
 * Frontends call sane_cancel() both to abort and at the end of every page --
 * scanimage --batch does the latter.  Tearing the batch down there would
 * throw away the rear side of the last sheet, so a cancel that arrives after
 * the image was read to EOF is treated as a page boundary, not an abort.
 */
void sane_cancel(SANE_Handle h)
{
	(void)h;
	if (cur_drained && !cur.data) {
		cur_drained = 0;
		return;                 /* normal end of page */
	}
	cur_drained = 0;
	drop_current();
	i2600_block_free(&pending_rear);
	if (batch_running) {
		i2600_scan_end(1);
		batch_running = 0;
	}
}

SANE_Status sane_set_io_mode(SANE_Handle h, SANE_Bool non_blocking)
{
	(void)h;
	return non_blocking ? SANE_STATUS_UNSUPPORTED : SANE_STATUS_GOOD;
}

SANE_Status sane_get_select_fd(SANE_Handle h, SANE_Int *fd)
{
	(void)h; (void)fd;
	return SANE_STATUS_UNSUPPORTED;
}

/*
 * libsane-dll looks the entry points up as _sane_<backend>_<op> first and
 * only then as the plain name, so export both spellings.  Backends in the
 * sane-backends tree get this from sanei_backend.h, which is not installed.
 */
#define I2600_ALIAS(op) \
	extern __typeof(sane_##op) sane_i2600_##op __attribute__((alias("sane_" #op))); \
	extern __typeof(sane_##op) _sane_i2600_##op __attribute__((alias("sane_" #op)))

I2600_ALIAS(init);
I2600_ALIAS(exit);
I2600_ALIAS(get_devices);
I2600_ALIAS(open);
I2600_ALIAS(close);
I2600_ALIAS(get_option_descriptor);
I2600_ALIAS(control_option);
I2600_ALIAS(get_parameters);
I2600_ALIAS(start);
I2600_ALIAS(read);
I2600_ALIAS(cancel);
I2600_ALIAS(set_io_mode);
I2600_ALIAS(get_select_fd);
