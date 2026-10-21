/*
 * i2600-grab -- fetch a raw image from a Kodak i2600 without the vendor driver.
 *
 * Proof of concept for a native SANE backend: speaks the USB vendor protocol
 * reconstructed in PROTOKOLL.md and INITIALISIERUNG.md directly via
 * libusb-1.0, reads the image blocks from bulk endpoint 0x82, parses the
 * backward TLV image footer and writes 8-bit grayscale PGM files.
 *
 * The device has no resolution, bit depth or color mode parameter: it always
 * delivers 300 dpi, 8 bit gray, 2580 pixels per line (INITIALISIERUNG.md
 * section 11).  Scaling, bit depth and bitonal conversion belong to the host.
 *
 * Build: make
 * Run:   ./i2600-grab [options]
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libusb-1.0/libusb.h>

#include "sequence.h"
#include "sequence-duplex.h"
#include "sequence-color.h"
#include "sequence-color-duplex.h"
#include "i2600-firmware.h"
#include "i2600-core.h"


#define USB_INTERFACE        0
#define EP_IMAGE_FRONT       0x82    /* bulk IN, front side image data   */
#define EP_IMAGE_REAR        0x86    /* bulk IN, rear side (i2600_duplex)      */
#define EP_BUTTON            0x81    /* interrupt IN, button events      */
#define EP_INTERRUPT         0x88    /* interrupt IN, 8-byte events      */

#define BULK_CHUNK           16384   /* 0x4000, the chunk size the device uses */
#define INTERRUPT_MSG_LEN    8

#define LINE_BYTES           I2600_LINE_BYTES

/* Interrupt event types, from DeviceManager::s_eventtidtable. */
#define EV_END_OF_OPERATION   0x01
#define EV_START_OF_OPERATION 0x02
#define EV_POWER_STATE        0x10
#define EV_TRANSPORT_STATE    0x12
#define EV_TRAY_STATE         0x13
#define EV_LAMP_STATE         0x15
#define EV_PAPER_JAM          0x30
#define EV_MULTIFEED          0x31
#define EV_BUFFER_OVERFLOW    0x32
#define EV_OTHER_ERROR        0x34
#define EV_PAGE_EXIT          0x41
#define EV_IMAGING_COMPLETE   0x42

/* ScannerOperation (0x10) wValue / wIndex as seen in the capture. */
#define SCANOP_STOP           0
#define SCANOP_START          1

/* The energy saver, which no capture contains: FCT::EnterEnergyStar() and
 * ExitEnergyStar() reach it through ScannerOperation 0x16 and 0x17, both of
 * which are this request with wValue 1 and 2 -- section 15.  Not in
 * sequence.h because that file is generated from the captures. */
#define OP_POWER_STATE        0x39

/* ------------------------------------------------------------------ state */

static libusb_device_handle *dev;
/*
 * Set as soon as any transfer reports the device gone.  A scanner switched
 * off at its power switch -- which is how a paper jam gets cleared here --
 * simply stops answering: the handle stays valid, every transfer returns
 * NO_DEVICE, and a daemon that only polls would spin on a dead device
 * forever.  Switching it back on enumerates a NEW device, so the only
 * recovery is to start over.  Front ends ask i2600_device_gone() and exit.
 */
static volatile int dev_gone;
int i2600_verbose;
int i2600_keep_going;
int i2600_duplex;        /* run the duplex sequence, read EP 0x86 too    */
int i2600_color;         /* run the colour sequence: 3 bytes per pixel   */
/* Which payload blob the active sequence indexes into. */
static const unsigned char *payload_base = i2600_payloads;
unsigned i2600_skip_mask;              /* bit per enum i2600_group */
const char *i2600_fw_source = I2600_FW_DEFAULT_SOURCE;
int i2600_fw_mode = 1;                 /* 0 = never, 1 = if needed, 2 = force */

struct ev_state {
	pthread_mutex_t lock;
	unsigned count[256];                /* events seen per type */
	uint8_t  id[256];
	uint16_t value1[256];
	uint32_t value2[256];
	/* Button presses as a queue, not just a counter: `id` tells the three
	 * panel keys apart, and a service that wants only the green one must
	 * not lose the id of a press to the next one.  See section 13. */
	struct { uint8_t id; uint16_t value1; } press[I2600_BUTTON_QUEUE];
	unsigned press_head, press_tail;     /* tail == head means empty */
	unsigned press_lost;
	int stop;
	int failed;
};
static struct ev_state ev;
static pthread_t ev_thread;
static int ev_thread_running;

static const char *event_name(uint8_t t)
{
	switch (t) {
	case 0x01: return "End of Operation";
	case 0x02: return "Start of Operation";
	case 0x03: return "Ding";
	case 0x10: return "Power State";
	case 0x11: return "Buffer State";
	case 0x12: return "Transport State";
	case 0x13: return "Tray State";
	case 0x14: return "Udds State";
	case 0x15: return "Lamp State";
	case 0x16: return "Interlock State";
	case 0x18: return "Scanner State";
	case 0x19: return "Elevator State";
	case 0x20: return "Button Press";
	case 0x30: return "Paper Jam";
	case 0x31: return "Multifeed";
	case 0x32: return "Buffer Overflow";
	case 0x34: return "Other Error";
	case 0x41: return "Page Exit";
	case 0x42: return "Imaging Complete";
	case 0x60: return "Screen Missing";
	case 0xf1: return "Trace Log";
	default:   return "?";
	}
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}
static double t_start;

void (*i2600_log_sink)(const char *line);

/*
 * A library must not write to stdout -- a SANE backend would corrupt the
 * image stream.  So: hand the line to a sink if one is installed, otherwise
 * print it only when a front end asked for progress output.
 */
static void logline(const char *fmt, ...)
{
	char line[1024];
	va_list ap;

	if (!i2600_log_sink && i2600_verbose < 1)
		return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (i2600_log_sink) {
		i2600_log_sink(line);
		return;
	}
	printf("[%7.3f] %s\n", now_s() - t_start, line);
	fflush(stdout);
}

static void msleep(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* ------------------------------------------------- interrupt event thread */

static void *interrupt_loop(void *arg)
{
	unsigned char buf[INTERRUPT_MSG_LEN];
	(void)arg;
	for (;;) {
		int n = 0, r;
		pthread_mutex_lock(&ev.lock);
		if (ev.stop) { pthread_mutex_unlock(&ev.lock); break; }
		pthread_mutex_unlock(&ev.lock);

		r = libusb_interrupt_transfer(dev, EP_INTERRUPT, buf, sizeof buf,
					      &n, 400);
		if (r == LIBUSB_ERROR_TIMEOUT)
			continue;
		if (r != 0) {
			if (r == LIBUSB_ERROR_NO_DEVICE) {
				dev_gone = 1;
				break;      /* nothing more will arrive */
			}
			if (r != LIBUSB_ERROR_INTERRUPTED)
				logline("EP88: %s", libusb_error_name(r));
			msleep(50);
			continue;
		}
		if (n < INTERRUPT_MSG_LEN)
			continue;
		{
			uint8_t type = buf[0], id = buf[1];
			uint16_t v1 = (uint16_t)(buf[2] | (buf[3] << 8));
			uint32_t v2 = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8)
				    | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
			pthread_mutex_lock(&ev.lock);
			ev.count[type]++;
			ev.id[type] = id;
			ev.value1[type] = v1;
			ev.value2[type] = v2;
			if (type == I2600_EV_BUTTON_PRESS) {
				unsigned next = (ev.press_head + 1) % I2600_BUTTON_QUEUE;
				if (next == ev.press_tail) {
					ev.press_lost++;   /* nobody is draining */
				} else {
					ev.press[ev.press_head].id = id;
					ev.press[ev.press_head].value1 = v1;
					ev.press_head = next;
				}
			}
			pthread_mutex_unlock(&ev.lock);
			logline("EP88  %02x %-20s id=%u value1=%u value2=%u",
				type, event_name(type), id, v1, v2);
		}
	}
	return NULL;
}

static unsigned ev_seen(uint8_t type)
{
	unsigned c;
	pthread_mutex_lock(&ev.lock);
	c = ev.count[type];
	pthread_mutex_unlock(&ev.lock);
	return c;
}

static uint16_t ev_value1(uint8_t type)
{
	uint16_t v;
	pthread_mutex_lock(&ev.lock);
	v = ev.value1[type];
	pthread_mutex_unlock(&ev.lock);
	return v;
}

/* Wait until event `type` has been seen more than `since` times. */
static int ev_wait(uint8_t type, unsigned since, long timeout_ms)
{
	long waited = 0;
	while (waited < timeout_ms) {
		if (ev_seen(type) > since)
			return 1;
		msleep(10);
		waited += 10;
	}
	logline("timeout after %ld ms waiting for event %02x %s",
		timeout_ms, type, event_name(type));
	return 0;
}

/* ----------------------------------------------------- control transfers */

static int ctrl_out(uint8_t opcode, uint16_t wvalue, uint16_t windex,
		    const unsigned char *data, uint16_t wlength)
{
	int r = libusb_control_transfer(dev, RT_HOST_TO_DEVICE, opcode, wvalue,
					windex, (unsigned char *)data, wlength, 2000);
	if (r == LIBUSB_ERROR_NO_DEVICE)
		dev_gone = 1;
	if (r < 0)
		logline("OUT  req=%02x val=%04x idx=%04x len=%u FAILED: %s",
			opcode, wvalue, windex, wlength, libusb_error_name(r));
	else if (i2600_verbose > 1)
		logline("OUT  req=%02x val=%04x idx=%04x len=%u ok",
			opcode, wvalue, windex, wlength);
	return r;
}

static int ctrl_in(uint8_t opcode, uint16_t wvalue, uint16_t windex,
		   unsigned char *data, uint16_t wlength)
{
	int r = libusb_control_transfer(dev, RT_DEVICE_TO_HOST, opcode, wvalue,
					windex, data, wlength, 2000);
	if (r == LIBUSB_ERROR_NO_DEVICE)
		dev_gone = 1;
	if (r < 0)
		logline("IN   req=%02x val=%04x idx=%04x len=%u FAILED: %s",
			opcode, wvalue, windex, wlength, libusb_error_name(r));
	else if (i2600_verbose > 1)
		logline("IN   req=%02x val=%04x idx=%04x len=%u -> %d bytes",
			opcode, wvalue, windex, wlength, r);
	return r;
}

/* ----------------------------------------------- shading calibration */

/*
 * The correction tables per sensor pixel live in the scanner's own flash at
 * 0x00000..0x3FFFF, but the FPGA applies them out of SDRAM, and the copy
 * flash -> SDRAM is the host's job.  What follows is
 * DeviceSettings::InitImageCalValues() and MoveFlashToSRAM() rebuilt; without
 * it every scan runs uncorrected and a white sheet clips at 255.  See
 * NATIVES-BACKEND.md section 17.
 */

/* Vendor request 0xe2 reads the device's SDRAM; no captured scan uses it, so
 * the generated headers do not name it. */
#define OP_SDRAM_MEMORY 0xe2

/* normalization_data_start and sdram_memory_address from the DMDATABASE
 * resource in devicemanager.so. */
#define CAL_NORMALIZATION_START   0x2a
#define CAL_SDRAM_ADDRESS         0x01ff9f00u
/* The last block of the calibration region holds DmPiranhaImageCalResults. */
#define CAL_RESULTS_ADDRESS       0x0003fc00u
#define CAL_RESULTS_BYTES         256
#define CAL_HAS_IMAGE_CAL         0xaa      /* its first byte, when valid */
/* VerifySramCalData() compares this much, every 32-bit word byte-reversed. */
#define CAL_VERIFY_BYTES          118

#define FPGA_TARGET_CODE          0xd800
#define FPGA_GENERAL_CONTROL      0xd901
#define FPGA_NORMALIZATION_START  0xd908
#define FPGA_SDRAM_DRIVER_ENABLE  0xdc4d
#define FPGA_FLASH_STATUS         0xde37
#define FPGA_FLASH_TO_SDRAM       0x10      /* general control: start the copy */
#define FPGA_FLASH_BUSY           0x01      /* flash status: still working */
#define CAL_FLASH_POLLS           100
#define CAL_FLASH_POLL_MS         30

/*
 * After a successful load the vendor also points the FPGA at the white patch,
 * the reference strip it averages beside the page.  white_patch_left_limit_front
 * from DMDATABASE; written as a high/low byte pair, the way
 * FpgaBase::SetHiLoPair() does it.  The rear pair at 0xdb0f/0xdb10 stays
 * untouched because the vendor's own run left it at zero on this scanner.
 */
#define FPGA_FRONT_WP_LEFT_LIMIT  0xda0f
#define CAL_WP_LEFT_LIMIT_FRONT   1326

#define SESSION_INFO_BYTES        64
#define SESSION_CAL_LOADED        1         /* the byte holding 0xAA */

static int cal_loaded;   /* 1 once the tables are known to be in SDRAM */

/* ReadFlashMemory/ReadSdramMemory split the address into the two fields. */
static int mem_in(uint8_t opcode, uint32_t addr, unsigned char *buf, uint16_t len)
{
	return ctrl_in(opcode, (uint16_t)(addr >> 16),
		       (uint16_t)(addr & 0xffff), buf, len);
}

static int fpga_in(uint16_t reg, unsigned char *value)
{
	return ctrl_in(OP_FPGA_REGISTER, reg, 0, value, 1);
}

static int fpga_out(uint16_t reg, unsigned char value)
{
	return ctrl_out(OP_FPGA_REGISTER, reg, 0, &value, 1);
}

/* VerifySramCalData(): 1 when the copy in SDRAM is the flash header again. */
static int cal_matches(const unsigned char *flash, const unsigned char *mem)
{
	unsigned i;

	for (i = 0; i < CAL_VERIFY_BYTES; i++) {
		unsigned word = i & ~3u;
		if (flash[i] != mem[word + 3 - (i - word)])
			return 0;
	}
	return 1;
}

static int cal_in_sdram(const unsigned char *flash)
{
	unsigned char mem[CAL_RESULTS_BYTES];

	if (mem_in(OP_SDRAM_MEMORY, CAL_SDRAM_ADDRESS, mem, sizeof mem) < 0)
		return -1;
	return cal_matches(flash, mem);
}

/*
 * MoveFlashToSRAM(): bit 4 of the general control register starts the copy,
 * the device does it itself and the host waits for the flash to go idle.
 * This writes FPGA registers only -- never flash, never NvRam.
 */
static int cal_copy_flash_to_sdram(void)
{
	unsigned char control = 0, status = 0;
	int i;

	if (fpga_out(FPGA_SDRAM_DRIVER_ENABLE, 1) < 0 ||
	    fpga_out(FPGA_TARGET_CODE, 8) < 0 ||
	    fpga_out(FPGA_NORMALIZATION_START, CAL_NORMALIZATION_START) < 0 ||
	    fpga_in(FPGA_GENERAL_CONTROL, &control) < 0 ||
	    fpga_out(FPGA_GENERAL_CONTROL,
		     (unsigned char)(control | FPGA_FLASH_TO_SDRAM)) < 0) {
		fpga_out(FPGA_TARGET_CODE, 0);
		return -1;
	}
	msleep(CAL_FLASH_POLL_MS);
	for (i = 0; i < CAL_FLASH_POLLS; i++) {
		if (fpga_in(FPGA_FLASH_STATUS, &status) < 0)
			break;
		msleep(CAL_FLASH_POLL_MS);
		if (!(status & FPGA_FLASH_BUSY))
			break;
	}
	/* Put the FPGA back where it was, whatever happened. */
	fpga_out(FPGA_GENERAL_CONTROL, control);
	fpga_out(FPGA_TARGET_CODE, 0);
	if (i >= CAL_FLASH_POLLS) {
		logline("calibration: timed out waiting for the flash to go idle");
		return -1;
	}
	return 0;
}

/*
 * Make sure the shading tables are in SDRAM.  Returns 1 when they are, 0 when
 * this scanner carries none in flash, -1 when a transfer failed.
 */
static int cal_ensure(void)
{
	unsigned char flash[CAL_RESULTS_BYTES];
	int in_sdram;

	if (cal_loaded)
		return 1;
	if (mem_in(OP_FLASH_MEMORY, CAL_RESULTS_ADDRESS, flash, sizeof flash) < 0)
		return -1;
	if (flash[0] != CAL_HAS_IMAGE_CAL) {
		logline("calibration: this scanner reports none in flash "
			"(marker %02x), images stay uncorrected", flash[0]);
		return 0;
	}
	if ((in_sdram = cal_in_sdram(flash)) < 0)
		return -1;
	if (!in_sdram) {
		logline("calibration: data in SDRAM is not the same as flash, "
			"copying it over");
		if (cal_copy_flash_to_sdram() < 0)
			return -1;
		if ((in_sdram = cal_in_sdram(flash)) < 0)
			return -1;
		if (!in_sdram) {
			logline("calibration: still not the same after the copy");
			return 0;
		}
	}
	logline("calibration: shading tables are in SDRAM");
	/* Measured: with the limit left at 0 the FPGA averages the dark
	 * transport border and reports a red/grey white patch average of 0;
	 * with 1326 it reports a plausible one.  The image is the same either
	 * way -- nothing we send feeds those averages back -- so this is here
	 * to leave the device in the state the vendor leaves it in. */
	fpga_out(FPGA_FRONT_WP_LEFT_LIMIT, CAL_WP_LEFT_LIMIT_FRONT >> 8);
	fpga_out(FPGA_FRONT_WP_LEFT_LIMIT + 1, CAL_WP_LEFT_LIMIT_FRONT & 0xff);
	cal_loaded = 1;
	return 1;
}

/*
 * Byte 1 of the session info block is a note the host leaves inside the
 * device: InitImageCalValues() returns straight away when it finds 0xAA
 * there.  The captured payload carries that 0xAA plus 60 bytes of another
 * scanner's state, so replaying it claims the calibration is loaded when it
 * is not -- and a vendor driver started afterwards believes the note and
 * skips its own load too.  So read the live block, load the calibration, and
 * only then set the note, exactly as the vendor does.
 */
static int session_info_write(void)
{
	unsigned char info[SESSION_INFO_BYTES];
	int r = ctrl_in(OP_SESSION_INFO, 0, 0, info, sizeof info);

	if (r < 0)
		return r;
	info[SESSION_CAL_LOADED] = (cal_ensure() == 1) ? CAL_HAS_IMAGE_CAL : 0;
	return ctrl_out(OP_SESSION_INFO, 0, 0, info, sizeof info);
}

/* Replay one generated table.  Returns 0 on success, -1 on a failed transfer. */
static int run_steps(const char *name, const struct i2600_step *steps, size_t n)
{
	unsigned char buf[2048];
	size_t i;
	int done = 0, skipped = 0;

	for (i = 0; i < n; i++) {
		const struct i2600_step *s = &steps[i];
		int r;

		if (i2600_skip_mask & (1u << s->group)) {
			skipped++;
			continue;
		}
		if (s->wlength > sizeof buf) {
			logline("step %s[%zu]: wLength %u exceeds buffer", name, i, s->wlength);
			return -1;
		}
		/* The one step whose recorded payload must not go out as it
		 * stands -- see session_info_write().  Matched on the request
		 * rather than the group, because only the simplex table keeps
		 * its groups apart. */
		if (s->bmrt == RT_HOST_TO_DEVICE &&
		    s->opcode == OP_SESSION_INFO && s->wlength == SESSION_INFO_BYTES)
			r = session_info_write();
		else if (s->bmrt == RT_HOST_TO_DEVICE)
			r = ctrl_out(s->opcode, s->wvalue, s->windex,
				     payload_base + s->payload, s->wlength);
		else
			r = ctrl_in(s->opcode, s->wvalue, s->windex, buf, s->wlength);

		if (r < 0) {
			logline("FAILED at %s[%zu] group=%s req=%02x val=%04x idx=%04x",
				name, i, i2600_group_name[s->group],
				s->opcode, s->wvalue, s->windex);
			if (!i2600_keep_going)
				return -1;
		}
		done++;
	}
	logline("phase %-12s %d transfers, %d skipped", name, done, skipped);
	return 0;
}

/* --i2600_duplex swaps the whole sequence, not just a few groups: the vendor
 * interleaves both cameras throughout.  See gen-i2600_duplex.py. */
#define N_OF(t) (sizeof (t) / sizeof (t)[0])
/* Four sequences, each generated position-faithfully from its own capture:
 * grey/colour times simplex/duplex.  Assembling one from parts of another
 * does not work -- see NATIVES-BACKEND.md sections 7 and 11. */
#define RUN(tbl) ((i2600_color && i2600_duplex) \
	? run_steps(#tbl, i2600_cd_##tbl,  N_OF(i2600_cd_##tbl)) \
	: i2600_color \
	? run_steps(#tbl, i2600_col_##tbl, N_OF(i2600_col_##tbl)) \
	: i2600_duplex \
	? run_steps(#tbl, i2600_dup_##tbl, N_OF(i2600_dup_##tbl)) \
	: run_steps(#tbl, i2600_seq_##tbl, N_OF(i2600_seq_##tbl)))

/* -------------------------------------------------------- image footer */

/* ImageFooterTags::ResolutionToDPI (devicemanager.so 0x11a3d0) */
int i2600_res_to_dpi(uint16_t code)
{
	static const int dpi[5] = { 600, 400, 300, 200, 150 };
	return code < 5 ? dpi[code] : -1;
}


#define BE16(p) ((uint16_t)(((p)[0] << 8) | (p)[1]))
#define BE32(p) (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) \
		 | ((uint32_t)(p)[2] << 8) | (uint32_t)(p)[3])

/*
 * Reimplementation of DeviceManager::Image::ParseImageFooter().  The footer
 * sits at the end of the block and is read BACKWARD; every record is 4 bytes,
 * <valueHi> <valueLo> <01> <tag>, value big endian.
 */
struct footer i2600_parse_footer(const unsigned char *buf, size_t len)
{
	struct footer f;
	long i = (long)len - 1;

	memset(&f, 0, sizeof f);
	while (i > 0) {
		uint8_t tag = buf[i];
		switch (tag) {
		case 0x00:                              /* padding */
			i -= 2;
			continue;
		case 0x01:                              /* start of footer */
			f.footer_bytes = len - (size_t)(i - 1);
			f.ok = 1;
			return f;
		case 0xff: {                            /* junk words + skip */
			long mult = buf[i - 1] * 2;
			f.junk_words = BE16(buf + i - 3);
			i = i - 4 - (long)f.junk_words * mult;
			continue;
		}
		case 0xfe: f.format      = BE16(buf + i - 3); i -= 4; continue;
		case 0xfd: f.type        = BE16(buf + i - 3); i -= 4; continue;
		case 0xfc: f.width_pairs = BE16(buf + i - 3);
			   f.width_pixels = (uint16_t)(f.width_pairs * 2);
			   i -= 4; continue;
		case 0xfa: f.xres_code   = BE16(buf + i - 3); i -= 4; continue;
		case 0xf9: f.yres_code   = BE16(buf + i - 3); i -= 4; continue;
		case 0xf8: i = i - 2 - 2 * buf[i - 1]; continue;   /* print string */
		case 0xf7: i -= 4; continue;                       /* ignored */
		case 0xf6: {
			uint16_t v = BE16(buf + i - 3);
			f.segment = v & 0x7fff;
			f.last_segment = (v & 0x8000) != 0;
			i -= 4; continue;
		}
		case 0xf5:                              /* u32 on the i2600 */
			f.document_count = BE32(buf + i - 5);
			i -= 6; continue;
		case 0xf4: i -= 6; continue;            /* two u16, unnamed */
		default:
			logline("footer: unknown tag 0x%02x at offset %ld", tag, i);
			return f;
		}
	}
	return f;
}

/* ------------------------------------------------------------ bulk read */


void i2600_block_free(struct block *b) { free(b->data); b->data = NULL; b->len = 0; }

/*
 * Read one logical image block: 16384-byte chunks until a short or empty
 * transfer.  first_timeout covers the wait for the device to start producing
 * data (paper feed), chunk_timeout the chunks inside a block.
 */
static int read_block(uint8_t endpoint, struct block *out,
		      unsigned first_timeout, unsigned chunk_timeout)
{
	size_t cap = 4u << 20;
	unsigned char *buf = malloc(cap);
	size_t len = 0;
	int chunks = 0;

	if (!buf)
		return -1;
	for (;;) {
		int n = 0, r;
		if (len + BULK_CHUNK > cap) {
			unsigned char *nb = realloc(buf, cap * 2);
			if (!nb) { free(buf); return -1; }
			buf = nb; cap *= 2;
		}
		r = libusb_bulk_transfer(dev, endpoint, buf + len, BULK_CHUNK, &n,
					 chunks == 0 ? first_timeout : chunk_timeout);
		if (r == LIBUSB_ERROR_TIMEOUT && n == 0) {
			if (chunks == 0) { free(buf); return 0; }   /* no block */
			logline("ep %02x: timeout after %d chunks (%zu bytes), "
				"treating as end of block", endpoint, chunks, len);
			break;
		}
		if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) {
			logline("ep %02x: bulk read failed after %zu bytes: %s",
				endpoint, len, libusb_error_name(r));
			if (len == 0) { free(buf); return -1; }
			break;
		}
		len += (size_t)n;
		chunks++;
		if (n < BULK_CHUNK)                   /* short packet = block end */
			break;
	}
	out->data = buf;
	out->len = len;
	logline("ep %02x: block of %zu bytes in %d transfers (len mod 508 = %zu)",
		endpoint, len, chunks, len % 508);
	return 1;
}

/*
 * Duplex: the device fills the front and rear buffers at the same time and
 * the vendor driver drains them in strict alternation -- in the capture a
 * 16384-byte chunk on 0x82 is followed within microseconds by one on 0x86.
 * Reading one endpoint to completion first would stall the other, so both
 * run concurrently: the rear side in its own thread, the front side here.
 */
struct read_job {
	uint8_t endpoint;
	struct block blk;
	unsigned first_timeout, chunk_timeout;
	int result;
};

static void *read_job_fn(void *arg)
{
	struct read_job *j = arg;
	j->result = read_block(j->endpoint, &j->blk, j->first_timeout, j->chunk_timeout);
	return NULL;
}

/*
 * Read the front block, and in i2600_duplex mode the rear block alongside it.
 * Returns the front result; the rear one is in rear->result (0 if no rear
 * read was started).
 */
static int read_pair(struct read_job *front, struct read_job *rear,
		     unsigned first_timeout, unsigned chunk_timeout)
{
	pthread_t th;
	int threaded = 0;

	front->endpoint = EP_IMAGE_FRONT;
	front->first_timeout = first_timeout;
	front->chunk_timeout = chunk_timeout;
	memset(&front->blk, 0, sizeof front->blk);

	memset(rear, 0, sizeof *rear);
	rear->endpoint = EP_IMAGE_REAR;
	rear->first_timeout = first_timeout;
	rear->chunk_timeout = chunk_timeout;

	if (i2600_duplex) {
		if (pthread_create(&th, NULL, read_job_fn, rear) == 0)
			threaded = 1;
		else
			logline("could not start the rear reader thread; front only");
	}

	front->result = read_block(front->endpoint, &front->blk,
				   first_timeout, chunk_timeout);
	if (threaded)
		pthread_join(th, NULL);
	return front->result;
}

/*
 * The rear camera looks at the sheet from the other side, so its image comes
 * out rotated by 180 degrees.  That rotation reverses the order of PIXELS --
 * reversing bytes would also swap the R and B channels of every colour
 * pixel, so the pixel size has to be respected.
 */
void i2600_rotate180(unsigned char *d, size_t bytes, unsigned bpp)
{
	size_t pixels = bytes / bpp, i, j;
	unsigned k;

	for (i = 0, j = pixels - 1; i < j; i++, j--)
		for (k = 0; k < bpp; k++) {
			unsigned char t = d[i * bpp + k];
			d[i * bpp + k] = d[j * bpp + k];
			d[j * bpp + k] = t;
		}
}

static int scanner_operation(uint16_t wvalue, uint16_t windex)
{
	return ctrl_out(OP_SCANNER_OPERATION, wvalue, windex, NULL, 0);
}

/*
 * Reference ("shutter") pass: the 0x30 block configures CaptureMode=2 with
 * FeedStyle=None, so the device exposes the dark reference without moving
 * paper.  51824 bytes, 20 lines.  Works with an empty feeder and is therefore
 * the cheapest end-to-end test of config + bulk + footer.
 */
/*
 * Reference ("shutter") pass: the 0x30 block configures CaptureMode=2 with
 * FeedStyle=None, so the device exposes the dark reference without moving
 * paper.  51824 bytes, 20 lines.  Works with an empty feeder and is therefore
 * the cheapest end-to-end test of config + bulk + footer.
 */
int i2600_reference(struct block *front, struct block *rear)
{
	struct read_job f, rr;
	unsigned soo, eoo;
	int r;

	logline("=== reference pass ===");
	if (RUN(ref_config) < 0)
		return -1;
	soo = ev_seen(EV_START_OF_OPERATION);
	if (scanner_operation(SCANOP_START, 0) < 0)
		return -1;
	ev_wait(EV_START_OF_OPERATION, soo, 3000);
	if (ctrl_out(OP_START_CAPTURE,
		     (i2600_color && i2600_duplex) ? I2600_CD_START_CAPTURE :
		     i2600_color  ? I2600_COLOR_START_CAPTURE  :
		     i2600_duplex ? I2600_DUPLEX_START_CAPTURE : 1, 0, NULL, 0) < 0)
		return -1;

	r = read_pair(&f, &rr, 5000, 2000);
	eoo = ev_seen(EV_END_OF_OPERATION);
	scanner_operation(SCANOP_STOP, 0);
	ev_wait(EV_END_OF_OPERATION, eoo, 2000);

	if (r <= 0) {
		logline("reference pass: no image block");
		i2600_block_free(&rr.blk);
		return -1;
	}
	*front = f.blk;
	if (rear)
		*rear = rr.blk;
	else
		i2600_block_free(&rr.blk);
	return 0;
}

/* ------------------------------------------------------------ scan batch */

static enum i2600_stop stop_reason = I2600_STOP_FEEDER_EMPTY;
static int pages_done;

enum i2600_stop i2600_scan_stop_reason(void) { return stop_reason; }

int i2600_scan_begin(void)
{
	logline("=== scan pass ===");
	pages_done = 0;
	stop_reason = I2600_STOP_FEEDER_EMPTY;
	if (RUN(scan_prep) < 0)
		return -1;
	if (RUN(scan_config) < 0)
		return -1;
	if (scanner_operation(SCANOP_START, 0) < 0)
		return -1;
	return 0;
}

/*
 * Fetch one sheet.  Returns 1 and fills front (and rear in duplex mode),
 * 0 when the batch is over -- ask i2600_scan_stop_reason() why -- or -1 on
 * a transport error.
 */
int i2600_scan_next_page(struct block *front, struct block *rear)
{
	struct read_job f, rr;
	int r = read_pair(&f, &rr, pages_done == 0 ? 20000 : 8000, 4000);

	if (r < 0) {
		i2600_block_free(&rr.blk);
		stop_reason = I2600_STOP_ERROR;
		return -1;
	}
	if (r == 0) {
		unsigned char st[32] = { 0 };
		i2600_block_free(&rr.blk);
		logline("no further image block (transport state=%u, pages exited=%u)",
			ev_value1(EV_TRANSPORT_STATE), ev_seen(EV_PAGE_EXIT));
		/* Say why we stopped: an empty feeder is the normal end, a
		 * blocked paper path is a jam that needs clearing by hand. */
		if (i2600_get_status(st) >= 0) {
			if (st[0x10] != 2) {
				stop_reason = I2600_STOP_PAPER_JAM;
				logline("PAPER JAM: paper path blocked (status[0x10]=%u); "
					"clear it by hand, a power cycle may be needed",
					st[0x10]);
			} else if (st[0x0f] == 1) {
				stop_reason = I2600_STOP_FEEDER_EMPTY;
				logline("feeder is empty -- normal end of batch");
			} else {
				stop_reason = I2600_STOP_STALLED;
				logline("feeder still holds paper (status[0x0f]=%u) but "
					"the device stopped producing", st[0x0f]);
			}
		}
		return 0;
	}
	pages_done++;
	*front = f.blk;
	if (rear) {
		*rear = rr.blk;
	} else {
		if (rr.result > 0)
			i2600_block_free(&rr.blk);
		else if (i2600_duplex)
			logline("page %d: no rear block", pages_done);
	}
	return 1;
}

/*
 * Stopping the transport while a sheet is still between the rollers leaves
 * the paper path blocked, and the device then answers every further start
 * with a Paper Jam event.  So ask the transport to stop and drain whatever
 * is still coming before leaving.
 */
void i2600_scan_end(int stop_transport)
{
	if (stop_transport) {
		logline("stopping the transport and draining");
		scanner_operation(SCANOP_STOP, 0);
		for (;;) {
			struct read_job df, dr;
			int more = read_pair(&df, &dr, 8000, 4000);
			if (more > 0)
				logline("drained %zu bytes", df.blk.len);
			if (dr.result > 0)
				logline("drained %zu rear bytes", dr.blk.len);
			i2600_block_free(&df.blk);
			i2600_block_free(&dr.blk);
			if (more <= 0 && dr.result <= 0)
				break;
		}
	}
	logline("scan pass: %d page(s)", pages_done);
}

/* ------------------------------------------------------------- firmware */

/*
 * A cold device runs its booter and answers nothing but GetStatus, so the
 * application firmware has to be downloaded before anything else -- see
 * NATIVES-BACKEND.md section 2.  Byte 0 of the status block says which
 * component is running.
 */
static int ensure_firmware(void)
{
	struct i2600_firmware fw;
	int comp = i2600_fw_component(dev);

	if (comp < 0) {
		logline("GetStatus failed: the device does not answer at all");
		return -1;
	}
	logline("status: component = %d (1=Booter 2=Loader 3=Scanner)", comp);

	if (comp == I2600_COMPONENT_SCANNER && i2600_fw_mode != 2)
		return 0;

	if (i2600_fw_mode == 0) {
		logline("the scanner firmware is not running and --no-firmware was "
			"given; the device cannot be used in this state");
		return -1;
	}

	if (i2600_fw_load(&fw, i2600_fw_source) < 0) {
		logline("no firmware images available; a cold device needs them. "
			"Point --firmware at the vendor's devicemanager.so or at a "
			"directory holding loader.bin, fpga.bin, scanner.bin and "
			"scanner0..3.bin");
		return -1;
	}
	logline("firmware images from %s: loader %u, fpga %u, scanner %u, "
		"banks %u/%u/%u/%u bytes", fw.source,
		fw.len[FW_LOADER], fw.len[FW_FPGA], fw.len[FW_SCANNER],
		fw.len[FW_SCANNER0], fw.len[FW_SCANNER1],
		fw.len[FW_SCANNER2], fw.len[FW_SCANNER3]);

	if (i2600_fw_download(dev, &fw, logline, i2600_verbose) < 0) {
		i2600_fw_free(&fw);
		return -1;
	}
	i2600_fw_free(&fw);
	return 0;
}

/* ------------------------------------------------------------- autocrop */

/*
 * The device hands over the whole transport window: the sheet sits inside a
 * markedly darker border (measured: row mean around 8 on the leader and
 * trailer against 135 on the page).  The vendor driver trims that in Perfect
 * Page on the host; this is our own, deliberately simple equivalent.
 *
 * The threshold is derived from the image rather than fixed, so a dark page
 * does not get mistaken for background: take the brightest decile of row
 * means as "this is what paper looks like here" and call everything below a
 * quarter of that background.  A page that fills the whole frame, or one too
 * dark to separate, makes this return -1 and the caller keeps the full frame.
 */
static int mean_cmp(const void *a, const void *b)
{
	unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
	return x < y ? -1 : x > y;
}

/*
 * Mean brightness of one row (horizontal = 1) or column, between `from` and
 * `to` inclusive along the other axis.  Sampled, not exhaustive -- this only
 * has to tell a shadowed edge from clean paper.
 */
static unsigned line_mean(const unsigned char *img, unsigned w, unsigned bpp,
			  int horizontal, unsigned at,
			  unsigned from, unsigned to)
{
	unsigned long s = 0, n = 0;
	unsigned i, step = (to - from) / 256 + 1;

	for (i = from; i <= to; i += step) {
		size_t off = horizontal
			? ((size_t)at * w + i) * bpp
			: ((size_t)i * w + at) * bpp;
		s += img[off];
		n++;
	}
	return n ? (unsigned)(s / n) : 0;
}

int i2600_autocrop(const unsigned char *img, unsigned w, unsigned lines,
		   unsigned bpp, struct i2600_crop *c)
{
	unsigned *rowmean, *sorted, thr, page;
	unsigned x, y, i, stride = w * bpp;
	int y0 = -1, y1 = -1, x0 = -1, x1 = -1;

	if (!img || !w || !lines || !bpp)
		return -1;
	rowmean = malloc((size_t)lines * sizeof *rowmean);
	sorted  = malloc((size_t)lines * sizeof *sorted);
	if (!rowmean || !sorted) { free(rowmean); free(sorted); return -1; }

	for (y = 0; y < lines; y++) {
		unsigned long s = 0;
		const unsigned char *row = img + (size_t)y * stride;
		for (i = 0; i < stride; i += bpp * 8)      /* every 8th pixel is plenty */
			s += row[i];
		rowmean[y] = (unsigned)(s / ((stride + bpp * 8 - 1) / (bpp * 8)));
	}
	memcpy(sorted, rowmean, (size_t)lines * sizeof *sorted);
	qsort(sorted, lines, sizeof *sorted, mean_cmp);
	page = sorted[(size_t)lines * 9 / 10];          /* brightest decile */
	thr  = page / 4;
	free(sorted);
	if (page < 40) { free(rowmean); return -1; }    /* too dark to separate */

	for (y = 0; y < lines; y++)
		if (rowmean[y] > thr) { if (y0 < 0) y0 = (int)y; y1 = (int)y; }
	free(rowmean);
	if (y0 < 0 || y1 <= y0)
		return -1;

	/* Columns, measured only inside the band we just found. */
	for (x = 0; x < w; x++) {
		unsigned long s = 0, n = 0;
		for (y = (unsigned)y0; y <= (unsigned)y1; y += 16) {
			s += img[(size_t)y * stride + (size_t)x * bpp];
			n++;
		}
		if (n && s / n > thr) { if (x0 < 0) x0 = (int)x; x1 = (int)x; }
	}
	if (x0 < 0 || x1 <= x0)
		return -1;

	/*
	 * Those bounds sit *in* the shadow at the sheet's edge, not past it.
	 * The threshold above is a quarter of the page white, and the edge
	 * gradient crosses that almost immediately -- measured on a real A4
	 * scan, the first ten columns ran 81, 121, 151, 172, 186, 192, 199,
	 * 218, 245, 254 before reaching paper.  So the crop kept a grey band
	 * about ten pixels wide on the left and right, and one or two rows top
	 * and bottom (87, 206, 251, ...).
	 *
	 * Walk each edge inwards while it is still darker than 90 % of the
	 * page white, which adapts to however wide the shadow happens to be.
	 * Capped at one per cent of the dimension so a page with genuinely
	 * dark content running to the edge loses at most 2 mm rather than
	 * being eaten from the side.
	 */
	{
		unsigned clean = page * 9 / 10;
		unsigned max_x = w / 100 + 1, max_y = lines / 100 + 1;
		unsigned n;

		for (n = 0; n < max_y && y0 < y1; n++, y0++)
			if (line_mean(img, w, bpp, 1, (unsigned)y0,
				      (unsigned)x0, (unsigned)x1) >= clean)
				break;
		for (n = 0; n < max_y && y1 > y0; n++, y1--)
			if (line_mean(img, w, bpp, 1, (unsigned)y1,
				      (unsigned)x0, (unsigned)x1) >= clean)
				break;
		for (n = 0; n < max_x && x0 < x1; n++, x0++)
			if (line_mean(img, w, bpp, 0, (unsigned)x0,
				      (unsigned)y0, (unsigned)y1) >= clean)
				break;
		for (n = 0; n < max_x && x1 > x0; n++, x1--)
			if (line_mean(img, w, bpp, 0, (unsigned)x1,
				      (unsigned)y0, (unsigned)y1) >= clean)
				break;
	}

	c->x0 = (unsigned)x0; c->y0 = (unsigned)y0;
	c->x1 = (unsigned)x1; c->y1 = (unsigned)y1;
	return 0;
}

/* Copy the cropped region in place; returns the new line count. */
unsigned i2600_apply_crop(unsigned char *img, unsigned w, unsigned bpp,
			  const struct i2600_crop *c, unsigned *new_width)
{
	unsigned cw = c->x1 - c->x0 + 1, ch = c->y1 - c->y0 + 1, y;
	unsigned stride = w * bpp, nstride = cw * bpp;

	for (y = 0; y < ch; y++)
		memmove(img + (size_t)y * nstride,
			img + (size_t)(c->y0 + y) * stride + (size_t)c->x0 * bpp,
			nstride);
	*new_width = cw;
	return ch;
}

/* ------------------------------------------------------------- lifecycle */

static libusb_context *usb_ctx;

/* Bytes per pixel on the wire: RGB in colour, 8 bit grey otherwise. */
unsigned i2600_bytes_per_pixel(void) { return i2600_color ? 3u : 1u; }

/*
 * The preconditions the vendor driver checks in Scanner::IsOkToStartScan()
 * before every batch (INITIALISIERUNG.md section 9).  Checking them up front
 * turns "the scan failed" into a precise message.
 */
enum i2600_ready i2600_check_ready(void)
{
	unsigned char st[32] = { 0 };

	if (ctrl_in(OP_GET_STATUS, 0, 0, st, sizeof st) < 0)
		return I2600_READY_IO_ERROR;
	if (st[0] != 3)
		return I2600_READY_COLD;         /* booter, no application firmware */
	if (st[0x13] == 2)
		return I2600_READY_COVER_OPEN;
	if (st[0x10] != 2)
		return I2600_READY_PAPER_JAM;
	if (st[0x0f] == 1)
		return I2600_READY_NO_DOCS;
	if (st[0x0e] != 0)
		return I2600_READY_BUSY;
	if (st[0x1f] & 0x80)
		return I2600_READY_CRITICAL;
	return I2600_READY_OK;
}

/*
 * Read one 8-byte message from the button channel (EP 0x81), which the
 * embedded pipe configuration flags OS_USBPIPEFLAGS_MONITORBUTTONEVENTS.
 * Returns the byte count, 0 on timeout, -1 on error.
 */
int i2600_read_button(unsigned char *buf8, unsigned timeout_ms)
{
	int n = 0, r;

	if (!dev)
		return -1;
	r = libusb_interrupt_transfer(dev, EP_BUTTON, buf8, 8, &n, timeout_ms);
	if (r == LIBUSB_ERROR_TIMEOUT)
		return 0;
	if (r != 0)
		return -1;
	return n;
}

/* How often an interrupt event has been seen, and its last id and value1. */
unsigned i2600_event_count(uint8_t type)  { return ev_seen(type); }
uint16_t i2600_event_value1(uint8_t type) { return ev_value1(type); }

uint8_t i2600_event_id(uint8_t type)
{
	uint8_t v;
	pthread_mutex_lock(&ev.lock);
	v = ev.id[type];
	pthread_mutex_unlock(&ev.lock);
	return v;
}

/*
 * Take the oldest pending button press off the queue.  Returns 1 and fills
 * the fields, or 0 when no press is waiting.  `id` names the key -- 1 is the
 * big green start button -- and `value1` is only the screen number, which is
 * the same for all three keys (section 13).
 */
int i2600_next_button(uint8_t *id, uint16_t *value1)
{
	int got = 0;

	pthread_mutex_lock(&ev.lock);
	if (ev.press_tail != ev.press_head) {
		if (id)     *id     = ev.press[ev.press_tail].id;
		if (value1) *value1 = ev.press[ev.press_tail].value1;
		ev.press_tail = (ev.press_tail + 1) % I2600_BUTTON_QUEUE;
		got = 1;
	}
	pthread_mutex_unlock(&ev.lock);
	return got;
}

void i2600_flush_buttons(void)
{
	pthread_mutex_lock(&ev.lock);
	ev.press_tail = ev.press_head;
	pthread_mutex_unlock(&ev.lock);
}

/*
 * The energy saver.  FCT::EnterEnergyStar() runs ScannerOperation 0x16,
 * which is bRequest 0x39 wValue 1; ExitEnergyStar() runs 0x17, wValue 2.
 * Unlike the open sequence these are single transfers, so they can be sent
 * to a sleeping device without waking it by accident -- section 15.
 */
int i2600_device_gone(void) { return dev_gone; }

/* ------------------------------------------------- waiting for the device */

/*
 * Nothing to do but wake the event loop, which then retries the open --
 * that retry, not this callback, is what decides the device is usable.
 * Returning 0 keeps the callback registered for a second arrival.
 */
static int LIBUSB_CALL on_arrival(libusb_context *c, libusb_device *d,
				  libusb_hotplug_event e, void *user)
{
	(void)c; (void)d; (void)e; (void)user;
	return 0;
}

/*
 * Block until the scanner is there and can actually be opened.
 *
 * Two things have to line up, and only the second one proves it: the device
 * must be on the bus, and udev's uaccess rule must have put the ACL on its
 * node (section 3).  Hotplug fires from libusb's own udev monitor and can
 * beat that ACL, so arrival is not enough -- the test is an open that
 * succeeds, retried for a few seconds.
 *
 * Without this a daemon can only exit and be restarted on a timer, which
 * means up to RestartSec of dead time after someone flips the power switch.
 *
 * Returns 0 once the device is openable, -1 on timeout (timeout_s 0 waits
 * forever) or when the wait itself failed.
 */
int i2600_wait_for_device(unsigned timeout_s)
{
	libusb_context *ctx = NULL;
	libusb_hotplug_callback_handle cb;
	double deadline = timeout_s ? now_s() + timeout_s : 0;
	int have_hotplug, registered = 0, rc = -1;

	if (libusb_init(&ctx) < 0)
		return -1;
	have_hotplug = libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) != 0;
	if (have_hotplug
	    && libusb_hotplug_register_callback(ctx,
			LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED,
			LIBUSB_HOTPLUG_NO_FLAGS, KODAK_VID, I2600_PID,
			LIBUSB_HOTPLUG_MATCH_ANY, on_arrival, NULL, &cb) == 0)
		registered = 1;
	else
		have_hotplug = 0;

	for (;;) {
		libusb_device_handle *h =
			libusb_open_device_with_vid_pid(ctx, KODAK_VID, I2600_PID);

		if (h) {                       /* on the bus and ours to use */
			libusb_close(h);
			rc = 0;
			break;
		}
		if (deadline && now_s() >= deadline)
			break;

		if (have_hotplug) {
			/*
			 * Sleep in libusb until something arrives.  The one
			 * second cap covers the ACL race: the callback may
			 * have fired already while the node was still
			 * root-only, so keep retrying the open regardless.
			 */
			struct timeval tv = { 1, 0 };
			libusb_handle_events_timeout_completed(ctx, &tv, NULL);
		} else {
			msleep(300);
		}
	}

	if (registered)
		libusb_hotplug_deregister_callback(ctx, cb);
	libusb_exit(ctx);
	return rc;
}

int i2600_power_saver(int on)
{
	return ctrl_out(OP_POWER_STATE, on ? 1 : 2, 0, NULL, 0);
}

/*
 * The lamp.  The prepare phase switches it on and the teardown off; the
 * energy saver leaves it alone, so a device parked asleep keeps glowing
 * unless it is told otherwise -- measured, section 16.
 */
int i2600_lamp(int on)
{
	return ctrl_out(OP_LAMP, on ? 1 : 0, 0, NULL, 0);
}

/* Scanner::GetPowerState() reads status byte 0x0c: 1 asleep, 3 idle, 4 scanning. */
int i2600_power_state(void)
{
	unsigned char st[32];

	if (i2600_get_status(st) < 0)
		return -1;
	return st[0x0c];
}

int i2600_vendor_out(uint8_t request, uint16_t wvalue, uint16_t windex,
		     const unsigned char *data, uint16_t len)
{
	return ctrl_out(request, wvalue, windex, data, len);
}

int i2600_vendor_in(uint8_t request, uint16_t wvalue, uint16_t windex,
		    unsigned char *data, uint16_t len)
{
	return ctrl_in(request, wvalue, windex, data, len);
}

/* Read a block from the operator panel (vendor request 0x62). */
int i2600_ocp_read(uint16_t wvalue, uint16_t windex, unsigned char *buf, unsigned len)
{
	return ctrl_in(OP_OCP_BLOCK, wvalue, windex, buf, len);
}

/*
 * Write one screen of the operator panel.  The panel is a 128x48 graphical
 * LCD in the usual page layout: six pages of eight stacked pixel rows, one
 * byte per column within a page, bit 0 topmost -- 768 bytes in all.  The
 * vendor renders text into exactly this with Pango and Cairo
 * (OCP::PopulateMessage) and ships it with OCP::LcdPopulate.
 *
 * The panel holds several numbered screens; SetButtonSequenceNumber picks
 * which one is shown, and the device draws that number in the corner itself.
 * wValue is (screen << 8) | region.  Regions group the messages: 2 are
 * operating states, 3 hardware faults, 4 the action to take.  The captures
 * only ever show region 4, but region 2 screen 1 holds "Scanning..." and
 * replacing it works -- see NATIVES-BACKEND.md section 14.
 * Note this is NOT the 0x0100|N encoding of the Screen Missing events; they
 * look similar and mixing them up costs an afternoon.
 */
int i2600_lcd_write(const unsigned char *bitmap, uint8_t screen, uint8_t region)
{
	return ctrl_out(OP_OCP_BLOCK, (uint16_t)((screen << 8) | region), 0x0680,
			bitmap, I2600_LCD_BYTES);
}

/*
 * Read and write the 128-byte NvRam block (vendor request 0x35, wValue 0).
 * It holds the mechanical calibration -- line widths, camera distances, left
 * pixels (section 8) -- so a write must never invent content: the vendor
 * itself only ever writes back what it just read, which is what commits a
 * freshly uploaded panel screen.
 */
int i2600_nvram_read(unsigned char *buf128)
{
	return ctrl_in(OP_NVRAM, 0, 0, buf128, I2600_NVRAM_BYTES);
}

int i2600_nvram_write(const unsigned char *buf128)
{
	return ctrl_out(OP_NVRAM, 0, 0, buf128, I2600_NVRAM_BYTES);
}

/* Start or stop interrupt event reporting; wIndex selects which events. */
int i2600_interrupt_events(uint16_t wvalue, uint16_t windex)
{
	return ctrl_out(OP_INTERRUPT_EVENTS, wvalue, windex, NULL, 0);
}

/* Arm the operator panel so button presses are reported. */
int i2600_buttons_enable(uint16_t wvalue, uint16_t windex)
{
	return ctrl_out(OP_SET_BUTTON_SEQUENCE, wvalue, windex, NULL, 0);
}

int i2600_get_status(unsigned char *buf32)
{
	return ctrl_in(OP_GET_STATUS, 0, 0, buf32, 32);
}

/*
 * Claim the device, bring up firmware if it is cold, start the interrupt
 * reader and run the open and prepare phases.  After this the device is
 * ready for i2600_reference() or i2600_scan_begin().
 */
int i2600_device_open(void)
{
	int r;

	if (i2600_color && i2600_duplex)
		payload_base = i2600_cd_payloads;
	else if (i2600_color)
		payload_base = i2600_color_payloads;
	else if (i2600_duplex)
		payload_base = i2600_duplex_payloads;
	t_start = now_s();
	/* A power cycle between two opens empties the SDRAM again, and the
	 * firmware download does too, so never trust an earlier load. */
	cal_loaded = 0;
	pthread_mutex_init(&ev.lock, NULL);

	if ((r = libusb_init(&usb_ctx)) < 0) {
		logline("libusb_init: %s", libusb_error_name(r));
		return -1;
	}
	if (i2600_verbose > 2)
		libusb_set_option(usb_ctx, LIBUSB_OPTION_LOG_LEVEL,
				  LIBUSB_LOG_LEVEL_DEBUG);
	dev = libusb_open_device_with_vid_pid(usb_ctx, KODAK_VID, I2600_PID);
	if (!dev) {
		logline("no Kodak i2600 (%04x:%04x) found, or no permission on "
			"its /dev/bus/usb node", KODAK_VID, I2600_PID);
		libusb_exit(usb_ctx);
		usb_ctx = NULL;
		return -1;
	}
	logline("opened %04x:%04x", KODAK_VID, I2600_PID);

	/* The vendor driver is a userspace libusb client, so normally no kernel
	 * driver is attached; ask anyway so a future usbfs-claiming module does
	 * not silently break us. */
	if (libusb_kernel_driver_active(dev, USB_INTERFACE) == 1) {
		logline("a kernel driver holds interface %d, detaching", USB_INTERFACE);
		if ((r = libusb_detach_kernel_driver(dev, USB_INTERFACE)) < 0)
			logline("detach failed: %s", libusb_error_name(r));
	}
	if ((r = libusb_claim_interface(dev, USB_INTERFACE)) < 0) {
		logline("claim interface %d: %s -- is the vendor driver or another "
			"scan running?", USB_INTERFACE, libusb_error_name(r));
		goto fail;
	}
	logline("claimed interface %d", USB_INTERFACE);

	if (ensure_firmware() < 0)
		goto fail_release;

	if (pthread_create(&ev_thread, NULL, interrupt_loop, NULL) == 0)
		ev_thread_running = 1;
	else
		logline("could not start the interrupt thread; continuing blind");

	if (RUN(open) < 0)
		goto fail_release;
	/* The vendor driver idles here for ~200 ms while the device reports
	 * End of Operation, Lamp State and two Ding events. */
	msleep(250);
	if (RUN(prepare) < 0)
		goto fail_release;
	/* Normally the session info step inside prepare has already done this;
	 * ask again so --skip session_w still gets a corrected image. */
	cal_ensure();
	return 0;

fail_release:
	libusb_release_interface(dev, USB_INTERFACE);
fail:
	libusb_close(dev);
	dev = NULL;
	libusb_exit(usb_ctx);
	usb_ctx = NULL;
	return -1;
}

/*
 * Run the prepare phase again on a device that is already open.
 *
 * The energy saver does not just idle the motor: coming back from it the
 * scanner has lost the camera and FPGA setup the prepare phase wrote, and
 * the lamp is off.  A scan started in that state runs the transport,
 * reports Page Exit for every sheet and hands back a formally correct block
 * of about 76 black lines instead of a page -- measured, see
 * NATIVES-BACKEND.md section 16.  So anything that sleeps the device has to
 * prepare it again before scanning.
 */
int i2600_device_prepare(void)
{
	if (!dev)
		return -1;
	logline("=== prepare again ===");
	return RUN(prepare);
}

void i2600_device_close(void)
{
	if (!dev)
		return;
	logline("=== teardown ===");
	RUN(teardown);
	if (ev_thread_running) {
		pthread_mutex_lock(&ev.lock);
		ev.stop = 1;
		pthread_mutex_unlock(&ev.lock);
		pthread_join(ev_thread, NULL);
		ev_thread_running = 0;
	}
	libusb_release_interface(dev, USB_INTERFACE);
	libusb_close(dev);
	dev = NULL;
	libusb_exit(usb_ctx);
	usb_ctx = NULL;
}
