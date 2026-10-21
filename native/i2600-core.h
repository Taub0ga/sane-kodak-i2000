/*
 * i2600-core -- the device half of the Kodak i2600 support: USB transport,
 * the vendor sequence, image blocks and their footer.  Both the i2600-grab
 * command line tool and the SANE backend sit on top of this.
 *
 * The protocol itself is documented in ../PROTOKOLL.md, ../INITIALISIERUNG.md
 * and NATIVES-BACKEND.md; this header only names what the two front ends use.
 */
#ifndef I2600_CORE_H
#define I2600_CORE_H

#include <stddef.h>
#include <stdint.h>

#define KODAK_VID       0x040a
#define I2600_PID       0x601d

/* The device scans in exactly one mode: 2580 pixels per line, 8 bit grey,
 * 300 dpi.  Resolution, bit depth and bitonal conversion are the host's job
 * -- see NATIVES-BACKEND.md section 3 of PROTOKOLL.md. */
#define I2600_LINE_BYTES   2580
#define I2600_DPI           300

/* One logical image block as it arrives on EP 0x82 / 0x86. */
struct block {
	unsigned char *data;
	size_t         len;
};

/* The TLV footer at the end of every block, read backwards. */
struct footer {
	int      ok;
	size_t   footer_bytes;
	uint16_t junk_words;
	uint16_t format;
	uint16_t type;
	uint16_t width_pairs;
	uint16_t width_pixels;
	uint16_t xres_code, yres_code;
	uint16_t segment;
	int      last_segment;
	uint32_t document_count;
};

/* Why a scan stopped, so a front end can tell the user something useful. */
enum i2600_stop {
	I2600_STOP_FEEDER_EMPTY = 0,
	I2600_STOP_PAPER_JAM,
	I2600_STOP_STALLED,          /* paper left, but the device went quiet */
	I2600_STOP_ERROR
};

/* Settings a front end may change before i2600_device_open(). */
extern int i2600_duplex;         /* also configure and read the rear camera */
extern int i2600_color;          /* run the colour sequence: 3 bytes per pixel */
extern int i2600_verbose;        /* 0 quiet, 1 progress, 2 every transfer    */
extern int i2600_keep_going;     /* do not abort on a failed control transfer */
extern unsigned i2600_skip_mask; /* groups to skip, see i2600_group_name[]   */
extern const char *i2600_fw_source;
extern int i2600_fw_mode;        /* 0 never download, 1 if cold, 2 always    */
extern void (*i2600_log_sink)(const char *line);  /* NULL = stderr */

/* Lifecycle.  open() claims the device, downloads firmware if the scanner is
 * cold, starts the interrupt reader and runs the open and prepare phases. */
int  i2600_device_open(void);
void i2600_device_close(void);

/*
 * Re-run the prepare phase without closing.  Needed after the energy saver,
 * which costs the device its camera and lamp setup -- section 16.
 */
int  i2600_device_prepare(void);

/*
 * True once a transfer found the device gone -- switched off, or unplugged.
 * Switching it on again enumerates a new device, so there is nothing to
 * reconnect to: the handle is dead and the process has to start over.
 */
int  i2600_device_gone(void);

/*
 * Block until the scanner is on the bus AND openable -- the second half
 * matters, because udev's ACL can lag the hotplug event.  timeout_s 0 waits
 * forever.  Lets a daemon sit through a power cycle instead of exiting and
 * being restarted on a timer.
 */
int  i2600_wait_for_device(unsigned timeout_s);

/* One reference ("shutter") pass: 20 lines, no paper movement. */
int  i2600_reference(struct block *front, struct block *rear);

/* A batch: begin, then next_page until it returns 0, then end. */
int  i2600_scan_begin(void);
int  i2600_scan_next_page(struct block *front, struct block *rear);
void i2600_scan_end(int stop_transport);
enum i2600_stop i2600_scan_stop_reason(void);

/* Block handling. */
struct footer i2600_parse_footer(const unsigned char *buf, size_t len);
void i2600_block_free(struct block *b);
void i2600_rotate180(unsigned char *d, size_t bytes, unsigned bpp);

/* Autocrop: find the sheet inside the transport window.  Returns 0 and the
 * inclusive bounds, or -1 when no page could be separated (keep the frame). */
struct i2600_crop { unsigned x0, y0, x1, y1; };
int      i2600_autocrop(const unsigned char *img, unsigned w, unsigned lines,
			unsigned bpp, struct i2600_crop *c);
unsigned i2600_apply_crop(unsigned char *img, unsigned w, unsigned bpp,
			  const struct i2600_crop *c, unsigned *new_width);

/* Why the scanner is not ready, mirroring the vendor's IsOkToStartScan(). */
enum i2600_ready {
	I2600_READY_OK = 0,
	I2600_READY_COLD,          /* booter running, firmware not downloaded */
	I2600_READY_COVER_OPEN,
	I2600_READY_PAPER_JAM,
	I2600_READY_NO_DOCS,
	I2600_READY_BUSY,          /* transport not idle */
	I2600_READY_CRITICAL,
	I2600_READY_IO_ERROR
};
enum i2600_ready i2600_check_ready(void);

/* Status block (vendor request 0x00 IN, 32 bytes). */
int  i2600_get_status(unsigned char *buf32);

/* Operator panel: arm button reporting, then read 8-byte messages from the
 * button channel (EP 0x81).  read returns the byte count, 0 on timeout. */
int  i2600_buttons_enable(uint16_t wvalue, uint16_t windex);
int  i2600_interrupt_events(uint16_t wvalue, uint16_t windex);

/* The operator panel's display: 128x48 pixels, six pages of eight stacked
 * rows, one byte per column within a page, bit 0 topmost. */
#define I2600_LCD_WIDTH  128
#define I2600_LCD_HEIGHT  48
#define I2600_LCD_BYTES  768
#define I2600_LCD_REGION   4    /* 2 = states, 3 = hardware faults, 4 = actions */
int  i2600_lcd_write(const unsigned char *bitmap, uint8_t screen, uint8_t region);

/* The calibration block.  Only ever write back what was read. */
#define I2600_NVRAM_BYTES 128
int  i2600_nvram_read(unsigned char *buf128);
int  i2600_nvram_write(const unsigned char *buf128);
int  i2600_ocp_read(uint16_t wvalue, uint16_t windex, unsigned char *buf, unsigned len);

/*
 * Generic vendor control transfers, for diagnostics and experiments.
 * Everything the device does rides on these; the named helpers are just the
 * ones whose meaning is pinned down.  NATIVES-BACKEND.md section 15 lists the
 * 27 ScannerOperation codes with their request and value.
 */
int  i2600_vendor_out(uint8_t request, uint16_t wvalue, uint16_t windex,
		      const unsigned char *data, uint16_t len);
int  i2600_vendor_in(uint8_t request, uint16_t wvalue, uint16_t windex,
		     unsigned char *data, uint16_t len);

/* Interrupt events seen so far.  Type 0x20 is a button press. */
#define I2600_EV_BUTTON_PRESS 0x20
unsigned i2600_event_count(uint8_t type);
uint16_t i2600_event_value1(uint8_t type);
uint8_t  i2600_event_id(uint8_t type);
int  i2600_read_button(unsigned char *buf8, unsigned timeout_ms);

/*
 * Button presses as a queue.  The three panel keys are told apart by `id`,
 * not by `value1`, which is only the screen number: id 1 is the big green
 * start button, 0 and 3 are the arrows -- see NATIVES-BACKEND.md section 13.
 * next_button() takes the oldest press and returns 1, or 0 when none waits.
 * Reporting only becomes live once i2600_device_open() has run; a process
 * that merely claims the interface and reads EP 0x88 receives nothing.
 */
#define I2600_BUTTON_QUEUE 16
int  i2600_next_button(uint8_t *id, uint16_t *value1);
void i2600_flush_buttons(void);

/*
 * The energy saver.  Presses still arrive while the device sleeps and do not
 * wake it, so a daemon can wait here -- section 15.  power_state() returns
 * status byte 0x0c: 1 asleep, 3 idle, 4 scanning, or -1 when it cannot be read.
 */
#define I2600_POWER_SAVER   1
#define I2600_POWER_IDLE    3
#define I2600_POWER_SCAN    4
int  i2600_power_saver(int on);
int  i2600_power_state(void);

/* The lamp, which the energy saver does not switch off by itself. */
int  i2600_lamp(int on);
int  i2600_res_to_dpi(uint16_t code);
unsigned i2600_bytes_per_pixel(void);

#endif /* I2600_CORE_H */
