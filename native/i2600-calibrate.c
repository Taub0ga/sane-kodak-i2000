/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Thomas Parthe */
/*
 * i2600-calibrate -- look at the scanner's per-pixel shading calibration and,
 * on request, reload it from the device's own flash into its SRAM.
 *
 * Nothing here ever writes flash.  The calibration tables live in the
 * scanner, in 256 KiB of SPI flash at 0x00000..0x3FFFF, with a 230-byte
 * result header in the last block at 0x3FC00 whose first byte is 0xAA when
 * the scanner holds valid image calibration.  The FPGA applies them out of
 * SRAM, and the copy flash -> SRAM is a separate step the host has to ask
 * for -- see NATIVES-BACKEND.md section 17.
 *
 * Requests used, all read-only except --load:
 *   0xe3 IN  wValue=addr>>16 wIndex=addr&0xffff   read flash
 *   0xe2 IN  likewise                             read SDRAM
 *   0xe1 IN  likewise                             read SRAM
 *   0xa3 IN/OUT wValue=register len=1             FPGA register
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <libusb.h>

#define OP_SRAM_MEMORY   0xe1
#define OP_SDRAM_MEMORY  0xe2
#define OP_FLASH_MEMORY  0xe3
#define OP_FPGA_REGISTER 0xa3
#define OP_SESSION_INFO  0x37

/* FPGA registers the vendor's MoveFlashToSRAM() touches. */
#define FPGA_TARGET_CODE             0xd800
#define FPGA_COMMON_GENERAL_CONTROL  0xd901
#define FPGA_NORMALIZATION_START     0xd908
#define FPGA_SDRAM_DRIVER_ENABLE     0xdc4d
#define FPGA_FLASH_STATUS            0xde37

/* From DMDATABASE in devicemanager.so: the i2000 family's two addresses. */
#define NORMALIZATION_DATA_START  0x2a
#define SDRAM_MEMORY_ADDRESS      0x01ff9f00u

#define CAL_FLASH_BASE    0x00000000u
#define CAL_FLASH_BYTES   0x00040000u      /* 128 blocks of 2 KiB */
#define CAL_BLOCK_BYTES   0x800
#define RESULTS_ADDR      0x0003fc00u      /* last block: the result header */
#define RESULTS_BYTES     0x100
#define RESULTS_USED      0xe6             /* DmPiranhaImageCalResults */

/* MoveFlashToSRAM waits for bit 0 of the flash status to clear. */
#define FLASH_BUSY_BIT    0x01
#define FLASH_POLL_MAX    100
#define FLASH_POLL_MS     30

static libusb_device_handle *h;

static void msleep(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static int vin(uint8_t request, uint16_t wvalue, uint16_t windex,
	       unsigned char *data, uint16_t len)
{
	return libusb_control_transfer(h, 0xC0, request, wvalue, windex,
				       data, len, 5000);
}

static int vout(uint8_t request, uint16_t wvalue, uint16_t windex,
		const unsigned char *data, uint16_t len)
{
	return libusb_control_transfer(h, 0x40, request, wvalue, windex,
				       (unsigned char *)data, len, 5000);
}

/* ReadFlashMemory/ReadSramMemory/ReadSdramMemory split the 32-bit address
 * into wValue = high half, wIndex = low half. */
static int mem_read(uint8_t request, uint32_t addr,
		    unsigned char *buf, uint16_t len)
{
	return vin(request, (uint16_t)(addr >> 16), (uint16_t)(addr & 0xffff),
		   buf, len);
}

static int fpga_get(uint16_t reg, unsigned char *value)
{
	return vin(OP_FPGA_REGISTER, reg, 0, value, 1);
}

static int fpga_set(uint16_t reg, unsigned char value)
{
	return vout(OP_FPGA_REGISTER, reg, 0, &value, 1);
}

static void hexdump(const unsigned char *b, unsigned n, unsigned base)
{
	unsigned i;
	for (i = 0; i < n; i++) {
		if (i % 16 == 0) printf("  +%03x  ", base + i);
		printf("%02x ", b[i]);
		if (i % 16 == 15) putchar('\n');
	}
	if (n % 16) putchar('\n');
}

/*
 * The vendor's VerifySramCalData(): compare the flash result header against
 * the copy in memory with every 32-bit word byte-reversed.  It checks 29 full
 * words (bytes 0x00..0x73) plus 0x74 and 0x75, so 118 bytes in all.  Returns
 * the number of mismatching bytes.
 */
#define VERIFY_BYTES 118

static int verify(const unsigned char *flash, const unsigned char *mem,
		  int report)
{
	int bad = 0;
	unsigned i;
	for (i = 0; i < VERIFY_BYTES; i++) {
		unsigned word = i & ~3u, swapped = word + (3 - (i - word));
		if (flash[i] == mem[swapped])
			continue;
		if (report && bad < 8)
			printf("    +0x%02x: flash %02x != memory[+0x%02x] "
			       "%02x\n", i, flash[i], swapped, mem[swapped]);
		bad++;
	}
	return bad;
}

static void show_fpga(void)
{
	static const struct { uint16_t reg; const char *name; } regs[] = {
		{ FPGA_TARGET_CODE,            "target code" },
		{ FPGA_COMMON_GENERAL_CONTROL, "common general control" },
		{ FPGA_NORMALIZATION_START,    "normalization data start" },
		{ FPGA_SDRAM_DRIVER_ENABLE,    "sdram driver enable" },
		{ FPGA_FLASH_STATUS,           "flash status" },
	};
	unsigned i;
	printf("FPGA registers\n");
	for (i = 0; i < sizeof regs / sizeof regs[0]; i++) {
		unsigned char v = 0;
		int r = fpga_get(regs[i].reg, &v);
		if (r < 0)
			printf("  %-26s 0x%04x  -- %s\n", regs[i].name,
			       regs[i].reg, libusb_error_name(r));
		else
			printf("  %-26s 0x%04x  = 0x%02x\n", regs[i].name,
			       regs[i].reg, v);
	}
	putchar('\n');
}

static int check(void)
{
	unsigned char flash[RESULTS_BYTES], mem[RESULTS_BYTES];
	int r, bad;

	show_fpga();

	r = mem_read(OP_FLASH_MEMORY, RESULTS_ADDR, flash, RESULTS_BYTES);
	if (r < 0) {
		fprintf(stderr, "read flash 0x%05x: %s\n", RESULTS_ADDR,
			libusb_error_name(r));
		return 1;
	}
	printf("image calibration results, flash 0x%05x (%d bytes read)\n",
	       RESULTS_ADDR, r);
	hexdump(flash, 64, 0);
	printf("  marker byte 0 = 0x%02x -- scanner %s image calibration\n\n",
	       flash[0], flash[0] == 0xaa ? "HAS" : "does NOT report");

	r = mem_read(OP_SDRAM_MEMORY, SDRAM_MEMORY_ADDRESS, mem, RESULTS_BYTES);
	if (r < 0) {
		printf("read sdram 0x%08x: %s -- trying SRAM instead\n",
		       SDRAM_MEMORY_ADDRESS, libusb_error_name(r));
		r = mem_read(OP_SRAM_MEMORY, SDRAM_MEMORY_ADDRESS, mem,
			     RESULTS_BYTES);
		if (r < 0) {
			fprintf(stderr, "read sram 0x%08x: %s\n",
				SDRAM_MEMORY_ADDRESS, libusb_error_name(r));
			return 1;
		}
	}
	printf("loaded copy, memory 0x%08x (%d bytes read)\n",
	       SDRAM_MEMORY_ADDRESS, r);
	hexdump(mem, 64, 0);

	bad = verify(flash, mem, 1);
	printf("\n  %d of %d compared bytes differ -- calibration data in "
	       "memory %s flash\n", bad, VERIFY_BYTES,
	       bad ? "is NOT the same as" : "matches");
	return bad ? 2 : 0;
}

/*
 * MoveFlashToSRAM(), transfer for transfer as devicemanager.so issues it.
 * Only FPGA registers are written; flash is read by the device itself.
 */
static int load(void)
{
	unsigned char ggc = 0, status = 0;
	int i, r;

	if ((r = fpga_set(FPGA_SDRAM_DRIVER_ENABLE, 1)) < 0) goto fail;
	if ((r = fpga_set(FPGA_TARGET_CODE, 8)) < 0) goto fail;
	if ((r = fpga_set(FPGA_NORMALIZATION_START,
			  NORMALIZATION_DATA_START)) < 0) goto fail;
	if ((r = fpga_get(FPGA_COMMON_GENERAL_CONTROL, &ggc)) < 0) goto fail;
	printf("common general control was 0x%02x, setting 0x%02x\n",
	       ggc, ggc | 0x10);
	if ((r = fpga_set(FPGA_COMMON_GENERAL_CONTROL,
			  (unsigned char)(ggc | 0x10))) < 0) goto fail;
	msleep(FLASH_POLL_MS);

	for (i = 0; i < FLASH_POLL_MAX; i++) {
		if ((r = fpga_get(FPGA_FLASH_STATUS, &status)) < 0) goto fail;
		msleep(FLASH_POLL_MS);
		if (!(status & FLASH_BUSY_BIT))
			break;
	}
	printf("flash status 0x%02x after %d polls\n", status, i + 1);
	/* Put the FPGA back where it was either way. */
	fpga_set(FPGA_COMMON_GENERAL_CONTROL, ggc);
	fpga_set(FPGA_TARGET_CODE, 0);

	if (i == FLASH_POLL_MAX) {
		fprintf(stderr, "timed out waiting for flash status to be "
				"idle\n");
		return 1;
	}
	printf("flash -> memory done, rechecking\n\n");
	return check();
fail:
	fprintf(stderr, "FPGA transfer failed: %s\n", libusb_error_name(r));
	fpga_set(FPGA_TARGET_CODE, 0);
	return 1;
}

/* A plain read of the whole 256 KiB calibration region, for a backup. */
static int dump(const char *path)
{
	unsigned char block[CAL_BLOCK_BYTES];
	uint32_t addr;
	FILE *f = fopen(path, "wb");

	if (!f) { perror(path); return 1; }
	for (addr = CAL_FLASH_BASE; addr < CAL_FLASH_BASE + CAL_FLASH_BYTES;
	     addr += CAL_BLOCK_BYTES) {
		int r = mem_read(OP_FLASH_MEMORY, addr, block, CAL_BLOCK_BYTES);
		if (r != (int)CAL_BLOCK_BYTES) {
			fprintf(stderr, "read flash 0x%05x: %s\n", addr,
				r < 0 ? libusb_error_name(r) : "short read");
			fclose(f);
			return 1;
		}
		if (fwrite(block, 1, CAL_BLOCK_BYTES, f) != CAL_BLOCK_BYTES) {
			perror(path); fclose(f); return 1;
		}
		if ((addr & 0x7fff) == 0)
			printf("\r  0x%05x", addr), fflush(stdout);
	}
	printf("\r  %u bytes written to %s\n", CAL_FLASH_BYTES, path);
	fclose(f);
	return 0;
}

/*
 * Byte 1 of the session info block is the vendor's "calibration is already in
 * memory" marker: InitImageCalValues() returns straight away when it reads
 * 0xAA there.  Our own sequence replays the captured 64 bytes, 0xAA included,
 * so it sets the marker without ever having loaded anything -- and the vendor
 * driver then skips its own load too.  See NATIVES-BACKEND.md section 17.
 */
static int session(void)
{
	unsigned char b[64];
	int r = vin(OP_SESSION_INFO, 0, 0, b, sizeof b);

	if (r < 0) {
		fprintf(stderr, "GetSessionInfo: %s\n", libusb_error_name(r));
		return 1;
	}
	printf("session info, request 0x37 (%d bytes)\n", r);
	hexdump(b, (unsigned)r, 0);
	printf("  byte 1 = 0x%02x -- host %s already loaded the calibration\n",
	       b[1], b[1] == 0xaa ? "claims it has" : "makes no claim that it");
	return 0;
}

/* A plain read of one or more FPGA registers, for pinning things down. */
static int peek_fpga(const char *reg_s, const char *len_s)
{
	unsigned reg = (unsigned)strtoul(reg_s, NULL, 0);
	unsigned len = len_s ? (unsigned)strtoul(len_s, NULL, 0) : 1;
	unsigned i;

	for (i = 0; i < len; i++) {
		unsigned char v = 0;
		int r = fpga_get((uint16_t)(reg + i), &v);
		if (r < 0)
			printf("  0x%04x  %s\n", reg + i, libusb_error_name(r));
		else
			printf("  0x%04x  = 0x%02x  (%u)\n", reg + i, v, v);
	}
	return 0;
}

/* A plain read of any of the three memories, for pinning things down. */
static int peek(const char *space, const char *addr_s, const char *len_s)
{
	unsigned char buf[4096];
	uint32_t addr = (uint32_t)strtoul(addr_s, NULL, 0);
	unsigned long len = len_s ? strtoul(len_s, NULL, 0) : 256;
	uint8_t request;
	int r;

	if (!strcmp(space, "flash"))      request = OP_FLASH_MEMORY;
	else if (!strcmp(space, "sram"))  request = OP_SRAM_MEMORY;
	else if (!strcmp(space, "sdram")) request = OP_SDRAM_MEMORY;
	else { fprintf(stderr, "unknown memory %s\n", space); return 1; }
	if (len == 0 || len > sizeof buf) len = sizeof buf;

	r = mem_read(request, addr, buf, (uint16_t)len);
	if (r < 0) {
		fprintf(stderr, "read %s 0x%08x: %s\n", space, addr,
			libusb_error_name(r));
		return 1;
	}
	printf("%s 0x%08x, request 0x%02x, %d bytes\n", space, addr, request, r);
	hexdump(buf, (unsigned)r, 0);
	return 0;
}

static void usage(void)
{
	fputs("usage: i2600-calibrate [--check | --load | --dump FILE]\n"
	      "\n"
	      "  --check  (default) read the calibration result header from\n"
	      "           flash and the copy in memory and compare them\n"
	      "  --load   ask the device to copy its calibration tables from\n"
	      "           flash into SRAM, then check again\n"
	      "  --dump   read the whole 256 KiB calibration region to a file\n"
	      "  --peek   SPACE ADDR [LEN]: read flash, sram or sdram raw\n"
	      "  --session  show the session info block and its 0xAA marker\n"
	      "  --fpga   REG [COUNT]: read FPGA registers, one byte each\n"
	      "\n"
	      "Nothing here writes flash.  Stop i2600-scand first:\n"
	      "  systemctl --user stop i2600-scand\n", stderr);
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	int rc;

	if (libusb_init(&ctx) < 0)
		return 1;
	h = libusb_open_device_with_vid_pid(ctx, 0x040a, 0x601d);
	if (!h) {
		fprintf(stderr, "no Kodak i2600 found\n");
		libusb_exit(ctx);
		return 1;
	}

	if (argc < 2 || !strcmp(argv[1], "--check"))
		rc = check();
	else if (!strcmp(argv[1], "--load"))
		rc = load();
	else if (!strcmp(argv[1], "--dump") && argc > 2)
		rc = dump(argv[2]);
	else if (!strcmp(argv[1], "--session"))
		rc = session();
	else if (!strcmp(argv[1], "--fpga") && argc > 2)
		rc = peek_fpga(argv[2], argc > 3 ? argv[3] : NULL);
	else if (!strcmp(argv[1], "--peek") && argc > 3)
		rc = peek(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
	else {
		usage();
		rc = 1;
	}

	libusb_close(h);
	libusb_exit(ctx);
	return rc;
}
