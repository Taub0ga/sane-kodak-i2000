/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Thomas Parthe */
/*
 * i2600-lcd -- store a 128x48 bitmap in one of the panel's screens.
 *
 * The panel is a graphical LCD and the device holds no fonts: the vendor
 * driver renders words with Pango and uploads the bitmap.  But an upload
 * only *stores* a screen, it does not draw it.  The device decides what is
 * on the panel, and it asks for a screen it does not have by raising a
 * Screen Missing event.  So uploading unsolicited is silent -- see
 * NATIVES-BACKEND.md section 14.  This tool is for filling a screen and
 * for experiments; tools/lcd-text.py makes a bitmap from a line of text.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "i2600-core.h"

int main(int argc, char **argv)
{
	unsigned char bmp[I2600_LCD_BYTES];
	unsigned screen = 9;          /* the screen the device shows by default */
	unsigned region = I2600_LCD_REGION;
	unsigned hold = 0;
	int select_screen = 0;   /* the shown screen usually already is ours */
	size_t n;
	FILE *f;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <bitmap file> [screen] [region] [hold seconds] [select 0|1]\n"
			"  %d bytes: %dx%d pixels, six pages of eight stacked rows,\n"
			"  one byte per column within a page, bit 0 topmost.\n"
			"  Use tools/lcd-text.py to make one from a line of text.\n",
			argv[0], I2600_LCD_BYTES, I2600_LCD_WIDTH, I2600_LCD_HEIGHT);
		return 2;
	}
	if (argc > 2) screen = (unsigned)strtoul(argv[2], NULL, 0);
	if (argc > 3) region = (unsigned)strtoul(argv[3], NULL, 0);
	if (argc > 4) hold = (unsigned)strtoul(argv[4], NULL, 0);
	if (argc > 5) select_screen = atoi(argv[5]);

	f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	n = fread(bmp, 1, sizeof bmp, f);
	fclose(f);
	if (n != sizeof bmp) {
		fprintf(stderr, "%s: %zu bytes, expected %d\n",
			argv[1], n, I2600_LCD_BYTES);
		return 1;
	}

	i2600_verbose = 1;
	if (i2600_device_open() < 0)
		return 1;
	if (i2600_lcd_write(bmp, (uint8_t)screen, (uint8_t)region) < 0) {
		fprintf(stderr, "upload failed\n");
		i2600_device_close();
		return 1;
	}
	printf("uploaded %s to panel screen %u (region %u)\n", argv[1], screen, region);

	/* The vendor writes the NvRam block back, unchanged, between the upload
	 * and the selection -- that appears to be what commits the screen.  We
	 * read it and write exactly those bytes back, never anything invented. */
	{
		unsigned char nv[I2600_NVRAM_BYTES];
		if (i2600_nvram_read(nv) == I2600_NVRAM_BYTES) {
			if (i2600_nvram_write(nv) < 0)
				printf("  (NvRam write-back failed)\n");
			else
				printf("NvRam block read and written back unchanged\n");
		} else {
			printf("  (NvRam read failed -- not writing)\n");
		}
	}

	/* The upload addresses a screen as (number << 8) | region.  The Screen
	 * Missing events report the same screen as 0x0100 | number instead --
	 * two encodings that look alike, and mixing them up costs an afternoon.
	 * Which screen is shown is chosen separately. */
	if (select_screen) {
		printf("selecting screen %u\n", screen);
		if (i2600_buttons_enable((uint16_t)screen, 0x01ff) < 0)
			printf("  (selection failed)\n");
	}
	/* Closing runs the teardown sequence and the device redraws its own
	 * screen, so hold the connection open while the image should stay up. */
	if (hold > 0) {
		/* Re-sending does not make an unsolicited upload appear; the
		 * device draws what it chooses.  It only keeps the screen filled
		 * in case the device asks for it while we hold the line. */
		struct timespec ts = { 0, 300 * 1000 * 1000 };
		unsigned ticks = hold * 1000 / 300, i, fails = 0;

		printf("re-sending every 300 ms for %u s -- look at the panel\n", hold);
		fflush(stdout);
		for (i = 0; i < ticks; i++) {
			nanosleep(&ts, NULL);
			if (i2600_lcd_write(bmp, (uint8_t)screen, (uint8_t)region) < 0)
				fails++;
			/* Re-select after every upload: the device only redraws
			 * when the shown screen is chosen, not when a buffer is
			 * filled behind it. */
			if (select_screen)
				i2600_buttons_enable((uint16_t)screen, 0x01ff);
		}
		printf("%u upload(s), %u failed\n", ticks, fails);
	}
	i2600_device_close();
	return 0;
}
