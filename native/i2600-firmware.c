/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Thomas Parthe */
/*
 * i2600-firmware -- firmware download for the Kodak i2600.
 *
 * Reimplementation of DeviceManager::InitializeScanner::LoadFirmware() and the
 * four routines it calls, from the vendor's devicemanager.so:
 *
 *   LoadLoader()            InitializeBuffer(LOADER)  + LoadInternalFirmware
 *   SetupFpga()             ProgramFpga() + TestFpgaMemory()   [test skipped]
 *   LoadBankedFirmware()    4x LoadFirmwareBank + LoadInternalFirmware(SCANNER)
 *
 * The scanner's main processor is a Cypress FX2: bRequest 0xa0 is the FX2's
 * own RW_INTERNAL request, which the USB core answers in hardware while the
 * 8051 is held in reset through its CPUCS register at 0xe600.  That is why a
 * cold device can be programmed although its booter answers nothing else.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "i2600-firmware.h"

/* ------------------------------------------------------------- protocol */

#define RT_HOST_TO_DEVICE   0x40    /* vendor, device, OUT */
#define RT_DEVICE_TO_HOST   0xc0    /* vendor, device, IN  */

#define OP_GET_STATUS                   0x00  /* DeviceSettings::GetStatus, 32 B */
#define OP_GET_FIRMWARE_VERSIONS        0x02  /* 56 B: booter|loader|scanner|... */
#define OP_FPGA_DOWNLOAD                0x20  /* ScannerOperation 0x11/0x12      */
#define OP_INTERNAL_FIRMWARE_DOWNLOAD   0x21  /* ScannerOperation 0x13           */
#define OP_LOAD_INTERNAL_FIRMWARE       0xa0  /* FX2 RW_INTERNAL                 */
#define OP_REGISTER_BLOCK               0xa3  /* also carries LoadFirmwareBank   */

#define FPGA_DOWNLOAD_START   1
#define FPGA_DOWNLOAD_STOP    0

/* Cypress FX2 control and status register; bit 0 holds the 8051 in reset. */
#define FX2_CPUCS             0xe600
#define FX2_CPUCS_8051_RESET  0x01

/* InitializeScanner chunks every control payload at 0x800 bytes. */
#define FW_CHUNK              0x800

/* LoadFirmwareBank: select the bank, then write into the 8051's banked code
 * window.  Both from the vendor's own log line
 * "Load Application(Bank:%d  Block:%d  Address:0x%04X  Bytes:%d)". */
#define FW_BANK_SELECT_REG    0xd80e
#define FW_BANK_WINDOW        0x4000

/* LoadInternalFirmware target addresses used by the vendor. */
#define FX2_INTERNAL_RAM      0x0000

/* ProgramFpga pushes the expanded bitstream out on this endpoint. */
#define EP_FIRMWARE_OUT       0x02

/*
 * PuffBuffer(): the Altera bitstream is expanded to one byte per bit, least
 * significant bit first, and padded with 300 zero bytes of trailing clocks.
 * (The vendor allocates 1000 extra bytes but only uses 300 -- its
 * a_ExtraPuffedBufferSize / a_UseExtraSize pair.)
 */
#define FPGA_PUFF_TRAILER     300

/* Host side only.  The vendor asks its USB layer for a chunk size
 * (EOSUSBIOCTL 0x1d, "SendBulkOut(chunk size=0x%x bytes)"); libusb has no such
 * limit, so we pick a size that keeps a single URB comfortably small. */
#define FPGA_BULK_CHUNK       32768

#define CTRL_TIMEOUT_MS       3000
#define BULK_TIMEOUT_MS       20000

/* How long to keep retrying GetStatus after the 8051 has been let out of
 * reset.  The vendor driver reads the status once and does not retry; we give
 * the new firmware a little room and say so if it needs it. */
#define RESTART_POLL_TRIES    20
#define RESTART_POLL_DELAY_MS 100

/* ------------------------------------------------------------- resources */

/*
 * Kodak modules carry their firmware as named resources appended behind the
 * last ELF section.  A 56-byte directory entry per resource,
 *
 *     char     name[32];
 *     uint64_t offset;      file offset of the payload
 *     uint64_t size;
 *     uint64_t reserved;
 *
 * followed by one more entry whose name is the sentinel "@!@kds@!@".  The
 * entries are stored in reverse payload order, so the directory is walked
 * backwards from the sentinel.  COsResource::Open() reaches them with the
 * pseudo module name "OsResourceGetLocal", i.e. "the resources of the module
 * I am linked into".
 */
#define KDS_RES_MAGIC      "@!@kds@!@"
#define KDS_RES_MAGIC_LEN  9
#define KDS_RES_ENTRY      56
#define KDS_RES_NAME       32

static const char *fw_resource_name[FW_IMAGE_COUNT] = {
	"LOADER", "FPGA", "SCANNER", "SCANNER0", "SCANNER1", "SCANNER2", "SCANNER3"
};
static const char *fw_file_name[FW_IMAGE_COUNT] = {
	"loader.bin", "fpga.bin", "scanner.bin",
	"scanner0.bin", "scanner1.bin", "scanner2.bin", "scanner3.bin"
};

static uint64_t le64(const unsigned char *p)
{
	uint64_t v = 0;
	int i;
	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static unsigned char *read_whole_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long size;

	if (!f) {
		fprintf(stderr, "firmware: %s: %s\n", path, strerror(errno));
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
		fprintf(stderr, "firmware: %s: cannot determine size\n", path);
		fclose(f);
		return NULL;
	}
	rewind(f);
	buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(f);
		fprintf(stderr, "firmware: out of memory for %s\n", path);
		return NULL;
	}
	if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
		fprintf(stderr, "firmware: %s: short read\n", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*out_len = (size_t)size;
	return buf;
}

/* Find the resource directory's sentinel, or (size_t)-1. */
static size_t find_res_directory_end(const unsigned char *d, size_t len)
{
	size_t i;

	if (len < KDS_RES_MAGIC_LEN)
		return (size_t)-1;
	for (i = len - KDS_RES_MAGIC_LEN + 1; i-- > 0; )
		if (!memcmp(d + i, KDS_RES_MAGIC, KDS_RES_MAGIC_LEN))
			return i;
	return (size_t)-1;
}

/* Locate one named resource in the directory.  Returns 0 on success. */
static int find_resource(const unsigned char *d, size_t len, size_t dir_end,
			 const char *name, size_t *off, uint32_t *size)
{
	size_t pos;

	for (pos = dir_end; pos >= KDS_RES_ENTRY; pos -= KDS_RES_ENTRY) {
		const unsigned char *e = d + pos - KDS_RES_ENTRY;
		uint64_t eoff, esize;
		size_t n;

		/* A non-printable or empty name ends the directory. */
		for (n = 0; n < KDS_RES_NAME && e[n]; n++)
			if (e[n] < 0x20 || e[n] > 0x7e)
				return -1;
		if (n == 0 || n == KDS_RES_NAME)
			return -1;

		eoff  = le64(e + KDS_RES_NAME);
		esize = le64(e + KDS_RES_NAME + 8);
		if (eoff == 0 || esize == 0 || eoff > len || esize > len - eoff)
			return -1;

		if (strlen(name) == n && !memcmp(e, name, n)) {
			*off  = (size_t)eoff;
			*size = (uint32_t)esize;
			return 0;
		}
	}
	return -1;
}

static int load_from_module(struct i2600_firmware *fw, const char *path)
{
	unsigned char *mod;
	size_t mod_len, dir_end;
	int i;

	mod = read_whole_file(path, &mod_len);
	if (!mod)
		return -1;

	dir_end = find_res_directory_end(mod, mod_len);
	if (dir_end == (size_t)-1) {
		fprintf(stderr, "firmware: %s carries no \"%s\" resource directory\n",
			path, KDS_RES_MAGIC);
		free(mod);
		return -1;
	}

	for (i = 0; i < FW_IMAGE_COUNT; i++) {
		size_t off;
		uint32_t size;

		if (find_resource(mod, mod_len, dir_end,
				  fw_resource_name[i], &off, &size) < 0) {
			fprintf(stderr, "firmware: resource %s not found in %s\n",
				fw_resource_name[i], path);
			goto fail;
		}
		fw->image[i] = malloc(size);
		if (!fw->image[i]) {
			fprintf(stderr, "firmware: out of memory for %s\n",
				fw_resource_name[i]);
			goto fail;
		}
		memcpy(fw->image[i], mod + off, size);
		fw->len[i] = size;
	}

	free(mod);
	return 0;
fail:
	free(mod);
	i2600_fw_free(fw);
	return -1;
}

static int load_from_directory(struct i2600_firmware *fw, const char *dir)
{
	int i;

	for (i = 0; i < FW_IMAGE_COUNT; i++) {
		char path[4096];
		size_t len;

		snprintf(path, sizeof path, "%s/%s", dir, fw_file_name[i]);
		fw->image[i] = read_whole_file(path, &len);
		if (!fw->image[i]) {
			i2600_fw_free(fw);
			return -1;
		}
		fw->len[i] = (uint32_t)len;
	}
	return 0;
}

int i2600_fw_load(struct i2600_firmware *fw, const char *source)
{
	struct stat st;
	int rc;

	memset(fw, 0, sizeof *fw);
	if (!source)
		source = I2600_FW_DEFAULT_SOURCE;
	snprintf(fw->source, sizeof fw->source, "%s", source);

	if (stat(source, &st) != 0) {
		fprintf(stderr, "firmware: %s: %s\n", source, strerror(errno));
		return -1;
	}
	rc = S_ISDIR(st.st_mode) ? load_from_directory(fw, source)
				 : load_from_module(fw, source);
	return rc;
}

void i2600_fw_free(struct i2600_firmware *fw)
{
	int i;
	for (i = 0; i < FW_IMAGE_COUNT; i++) {
		free(fw->image[i]);
		fw->image[i] = NULL;
		fw->len[i] = 0;
	}
}

/* ------------------------------------------------------------- transfers */

struct fwctx {
	libusb_device_handle *h;
	void (*log)(const char *fmt, ...);
	int verbose;
};

static void fwlog(const struct fwctx *c, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	if (!c->log)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	c->log("%s", buf);
}

static void fw_msleep(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static int fw_out(const struct fwctx *c, uint8_t opcode, uint16_t wvalue,
		  uint16_t windex, const unsigned char *data, uint16_t wlength)
{
	int r = libusb_control_transfer(c->h, RT_HOST_TO_DEVICE, opcode, wvalue,
					windex, (unsigned char *)data, wlength,
					CTRL_TIMEOUT_MS);
	if (r < 0)
		fwlog(c, "firmware: OUT req=%02x val=%04x idx=%04x len=%u FAILED: %s",
		      opcode, wvalue, windex, wlength, libusb_error_name(r));
	else if (c->verbose > 1)
		fwlog(c, "firmware: OUT req=%02x val=%04x idx=%04x len=%u ok",
		      opcode, wvalue, windex, wlength);
	return r;
}

static int fw_in(const struct fwctx *c, uint8_t opcode, uint16_t wvalue,
		 uint16_t windex, unsigned char *data, uint16_t wlength)
{
	return libusb_control_transfer(c->h, RT_DEVICE_TO_HOST, opcode, wvalue,
				       windex, data, wlength, CTRL_TIMEOUT_MS);
}

/* DeviceSettings::ScannerOperation(): opcode with wValue, no payload. */
static int scanner_op(const struct fwctx *c, uint8_t opcode, uint16_t wvalue)
{
	return fw_out(c, opcode, wvalue, 0, NULL, 0);
}

int i2600_fw_component(libusb_device_handle *h)
{
	unsigned char status[32];
	int r = libusb_control_transfer(h, RT_DEVICE_TO_HOST, OP_GET_STATUS, 0, 0,
					status, sizeof status, CTRL_TIMEOUT_MS);
	if (r != (int)sizeof status)
		return -1;
	return status[0];
}

/* Status bytes 1..4 are the running component's version, u32 big endian. */
static uint32_t fw_version(libusb_device_handle *h)
{
	unsigned char status[32];
	int r = libusb_control_transfer(h, RT_DEVICE_TO_HOST, OP_GET_STATUS, 0, 0,
					status, sizeof status, CTRL_TIMEOUT_MS);
	if (r != (int)sizeof status)
		return 0;
	return ((uint32_t)status[1] << 24) | ((uint32_t)status[2] << 16) |
	       ((uint32_t)status[3] << 8) | status[4];
}

/*
 * Wait until the device reports the expected component.  LoadFirmware itself
 * polls nothing, but the 8051 needs a moment after its reset is released and
 * a cold device is the one case the vendor never had to survive on this host.
 */
static int wait_for_component(const struct fwctx *c, int want)
{
	int i;

	for (i = 0; i < RESTART_POLL_TRIES; i++) {
		int comp = i2600_fw_component(c->h);
		if (comp == want) {
			if (i)
				fwlog(c, "firmware: component %d after %d ms",
				      want, i * RESTART_POLL_DELAY_MS);
			return 0;
		}
		fw_msleep(RESTART_POLL_DELAY_MS);
	}
	fwlog(c, "firmware: device did not reach component %d (now %d)",
	      want, i2600_fw_component(c->h));
	return -1;
}

/* ------------------------------------------- InitializeScanner primitives */

/*
 * LoadInternalFirmware(data, len, addr, a_bAlreadyInReset, a_bLeaveInReset).
 *
 * Holds the FX2's 8051 in reset, writes the image into its internal RAM in
 * 0x800-byte control payloads, then lets it run again.  The trailing
 * zero-length write is the vendor's loop exit condition; it is reproduced
 * because the firmware may well use it as an end marker.
 */
static int load_internal_firmware(const struct fwctx *c, const unsigned char *data,
				  uint32_t len, uint16_t addr,
				  int already_in_reset, int leave_in_reset)
{
	unsigned char reset_on = FX2_CPUCS_8051_RESET, reset_off = 0;
	uint32_t off = 0, left = len;

	if (!already_in_reset) {
		/* ScannerOperation 0x13.  The booter answers nothing but GetStatus,
		 * so this one may well stall on a cold device; the FX2's own 0xa0
		 * request below is handled by the USB core either way, so a stall
		 * here is logged and ignored rather than treated as fatal. */
		if (scanner_op(c, OP_INTERNAL_FIRMWARE_DOWNLOAD, 0) < 0)
			fwlog(c, "firmware: ScannerOperation 0x21 refused, continuing");
		fwlog(c, "firmware: hold FX2 reset");
		if (fw_out(c, OP_LOAD_INTERNAL_FIRMWARE, FX2_CPUCS, 0,
			   &reset_on, 1) < 0)
			return -1;
	}

	for (;;) {
		uint16_t n = left > FW_CHUNK ? FW_CHUNK : (uint16_t)left;

		left -= n;
		if (c->verbose)
			fwlog(c, "firmware: LoadInternalFirmware(SrcIdx:%u  Bytes:%u  "
			      "TargetAddr:0x%04X)", off, n, (unsigned)(addr + off));
		if (fw_out(c, OP_LOAD_INTERNAL_FIRMWARE, (uint16_t)(addr + off), 0,
			   data + off, n) < 0)
			return -1;
		if (n == 0)
			break;
		off += n;
	}

	if (!leave_in_reset) {
		fwlog(c, "firmware: release FX2 reset");
		if (fw_out(c, OP_LOAD_INTERNAL_FIRMWARE, FX2_CPUCS, 0,
			   &reset_off, 1) < 0)
			return -1;
	}
	return 0;
}

/*
 * LoadFirmwareBank(bank, data, len): select the code bank through FPGA
 * register 0xd80e, then write the image into the 8051's banked code window at
 * 0x4000 in 0x800-byte pieces.
 */
static int load_firmware_bank(const struct fwctx *c, uint8_t bank,
			      const unsigned char *data, uint32_t len)
{
	uint32_t off = 0, left = len;
	unsigned block = 0;

	if (fw_out(c, OP_REGISTER_BLOCK, FW_BANK_SELECT_REG, 0, &bank, 1) < 0) {
		fwlog(c, "firmware: unable to select firmware bank %u", bank);
		return -1;
	}

	for (;;) {
		uint16_t n = left > FW_CHUNK ? FW_CHUNK : (uint16_t)left;

		left -= n;
		if (c->verbose)
			fwlog(c, "firmware: Load Application(Bank:%u  Block:%u  "
			      "Address:0x%04X  Bytes:%u)", bank, block,
			      (unsigned)(FW_BANK_WINDOW + off), n);
		if (fw_out(c, OP_REGISTER_BLOCK, (uint16_t)(FW_BANK_WINDOW + off),
			   (uint16_t)off, data + off, n) < 0) {
			fwlog(c, "firmware: unable to load firmware segment %u", block);
			return -1;
		}
		block++;
		if (n == 0)
			break;
		off += n;
	}
	return 0;
}

/* PuffBuffer(): one byte per bitstream bit, LSB first, plus trailing zeros. */
static unsigned char *puff_buffer(const unsigned char *in, uint32_t in_len,
				  uint32_t *out_len)
{
	size_t n = (size_t)in_len * 8;
	unsigned char *out = calloc(n + FPGA_PUFF_TRAILER, 1);
	uint32_t i;
	int b;

	if (!out)
		return NULL;
	for (i = 0; i < in_len; i++)
		for (b = 0; b < 8; b++)
			out[(size_t)i * 8 + b] = (in[i] >> b) & 1;
	*out_len = (uint32_t)(n + FPGA_PUFF_TRAILER);
	return out;
}

/*
 * ProgramFpga(): expand the bitstream, bracket the transfer with
 * ScannerOperation 0x11 / 0x12 (opcode 0x20, wValue 1 then 0) and push the
 * expanded stream out on bulk endpoint 0x02.
 */
static int program_fpga(const struct fwctx *c, const unsigned char *data, uint32_t len)
{
	unsigned char *puffed;
	uint32_t puffed_len = 0, off = 0;
	int rc = -1;

	puffed = puff_buffer(data, len, &puffed_len);
	if (!puffed) {
		fwlog(c, "firmware: unable to expand FPGA firmware");
		return -1;
	}
	fwlog(c, "firmware: FPGA bitstream %u bytes -> %u expanded bytes",
	      len, puffed_len);

	if (scanner_op(c, OP_FPGA_DOWNLOAD, FPGA_DOWNLOAD_START) < 0) {
		fwlog(c, "firmware: unable to send FPGA download start command");
		goto out;
	}

	while (off < puffed_len) {
		int n = (int)(puffed_len - off);
		int sent = 0, r;

		if (n > FPGA_BULK_CHUNK)
			n = FPGA_BULK_CHUNK;
		r = libusb_bulk_transfer(c->h, EP_FIRMWARE_OUT, puffed + off, n,
					 &sent, BULK_TIMEOUT_MS);
		if (r < 0) {
			fwlog(c, "firmware: bulk out at %u/%u failed: %s",
			      off, puffed_len, libusb_error_name(r));
			goto stop;
		}
		if (sent == 0) {
			fwlog(c, "firmware: bulk out stalled at %u/%u", off, puffed_len);
			goto stop;
		}
		off += (uint32_t)sent;
	}
	rc = 0;

stop:
	if (scanner_op(c, OP_FPGA_DOWNLOAD, FPGA_DOWNLOAD_STOP) < 0) {
		fwlog(c, "firmware: unable to send FPGA download stop command");
		rc = -1;
	}
	/* ProgramFpga's "Resetting all pipes" before it returns. */
	libusb_clear_halt(c->h, EP_FIRMWARE_OUT);
out:
	free(puffed);
	return rc;
}

/* ------------------------------------------------------------- the whole */

int i2600_fw_download(libusb_device_handle *h, const struct i2600_firmware *fw,
		      void (*log)(const char *fmt, ...), int verbose)
{
	struct fwctx ctx = { h, log, verbose };
	const struct fwctx *c = &ctx;
	unsigned char versions[56];
	int comp, i, r;

	comp = i2600_fw_component(h);
	if (comp < 0) {
		fwlog(c, "firmware: GetStatus failed -- device not usable");
		return -1;
	}
	if (comp == I2600_COMPONENT_SCANNER) {
		fwlog(c, "firmware: scanner firmware already running (version %08X)",
		      fw_version(h));
		return 0;
	}
	if (comp != I2600_COMPONENT_BOOTER && comp != I2600_COMPONENT_LOADER) {
		fwlog(c, "firmware: unknown component %d running, refusing to download",
		      comp);
		return -1;
	}

	fwlog(c, "firmware: component %d (%s) version %08X -- downloading from %s",
	      comp, comp == I2600_COMPONENT_BOOTER ? "booter" : "loader",
	      fw_version(h), fw->source);

	/* LoadLoader() */
	fwlog(c, "firmware: LoadLoader (%u bytes)", fw->len[FW_LOADER]);
	if (load_internal_firmware(c, fw->image[FW_LOADER], fw->len[FW_LOADER],
				   FX2_INTERNAL_RAM, 0, 0) < 0)
		return -1;
	if (wait_for_component(c, I2600_COMPONENT_LOADER) < 0) {
		fwlog(c, "firmware: unable to load the Loader");
		return -1;
	}
	fwlog(c, "firmware: loader running, version %08X", fw_version(h));

	/* SetupFpga() -> ProgramFpga().  TestFpgaMemory() is deliberately
	 * skipped: it is a diagnostic, not a precondition. */
	fwlog(c, "firmware: ProgramFpga");
	if (program_fpga(c, fw->image[FW_FPGA], fw->len[FW_FPGA]) < 0) {
		fwlog(c, "firmware: failed to program the FPGA");
		return -1;
	}

	/* LoadBankedFirmware() */
	for (i = 0; i < 4; i++) {
		int img = FW_SCANNER0 + i;

		fwlog(c, "firmware: LoadFirmwareBank %d (%u bytes)", i, fw->len[img]);
		if (load_firmware_bank(c, (uint8_t)i, fw->image[img], fw->len[img]) < 0)
			return -1;
	}
	fwlog(c, "firmware: LoadInternalFirmware SCANNER (%u bytes)",
	      fw->len[FW_SCANNER]);
	if (load_internal_firmware(c, fw->image[FW_SCANNER], fw->len[FW_SCANNER],
				   FX2_INTERNAL_RAM, 0, 0) < 0)
		return -1;

	if (wait_for_component(c, I2600_COMPONENT_SCANNER) < 0) {
		fwlog(c, "firmware: failed to start the Scanner Firmware");
		return -1;
	}
	fwlog(c, "firmware: scanner firmware running, version %08X", fw_version(h));

	/* The vendor reads GetFirmwareVersions right after
	 * (CheckAllFirmwareVersions); it also proves the device now answers
	 * requests other than GetStatus. */
	r = fw_in(c, OP_GET_FIRMWARE_VERSIONS, 0, 0, versions, sizeof versions);
	if (r == (int)sizeof versions)
		fwlog(c, "firmware: versions booter=%02X%02X%02X%02X "
		      "loader=%02X%02X%02X%02X scanner=%02X%02X%02X%02X",
		      versions[0], versions[1], versions[2], versions[3],
		      versions[4], versions[5], versions[6], versions[7],
		      versions[8], versions[9], versions[10], versions[11]);
	else
		fwlog(c, "firmware: GetFirmwareVersions still fails (%d)", r);

	return 0;
}
