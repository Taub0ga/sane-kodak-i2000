/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Thomas Parthe */
/*
 * i2600-status -- read the scanner's 32-byte status block (vendor request
 * 0x00 IN) and decode the bytes that Scanner::IsOkToStartScan() checks.
 * See INITIALISIERUNG.md section 9 for where the offsets come from.
 */
#include <stdio.h>
#include <libusb.h>

int main(void)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	unsigned char b[32] = { 0 };
	int r, i;

	if (libusb_init(&ctx) < 0)
		return 1;
	h = libusb_open_device_with_vid_pid(ctx, 0x040a, 0x601d);
	if (!h) {
		fprintf(stderr, "no Kodak i2600 found\n");
		libusb_exit(ctx);
		return 1;
	}
	r = libusb_control_transfer(h, 0xC0, 0x00, 0, 0, b, sizeof b, 2000);
	if (r < 0) {
		fprintf(stderr, "GetStatus: %s\n", libusb_error_name(r));
		libusb_close(h);
		libusb_exit(ctx);
		return 1;
	}
	for (i = 0; i < r; i++) {
		if (i % 16 == 0)
			printf("  +%02x  ", i);
		printf("%02x ", b[i]);
		if (i % 16 == 15)
			putchar('\n');
	}
	if (r % 16)
		putchar('\n');

	printf("\n  component  [0x00] = %u   (1 booter, 2 loader, 3 scanner)\n", b[0]);
	printf("  transport  [0x0e] = %u   (0 = idle, ready to scan)\n", b[0x0e]);
	printf("  ADF        [0x0f] = %u   (1 = empty)\n", b[0x0f]);
	printf("  paper path [0x10] = %u   (2 = clear, else jammed at UDDS)\n", b[0x10]);
	printf("  cover      [0x13] = %u   (2 = open)\n", b[0x13]);
	printf("  power      [0x0c] = %u   (1 = energy saver, 3 = idle, 4 = scanning)\n", b[0x0c]);
	printf("  critical   [0x1f] = 0x%02x (bit 7 = critical error)\n", b[0x1f]);
	/* Only the scanner firmware fills the status block; a cold device
	 * answers GetStatus from its booter with everything but byte 0 zero,
	 * so the checks below would be meaningless. */
	if (b[0] != 3)
		printf("\n  device is cold (component %u) -- the fields above are "
		       "not populated yet\n", b[0]);
	else
		printf("\n  ok to start a scan: %s\n",
		       (b[0x13] != 2 && b[0x10] == 2 && b[0x0e] == 0
			&& !(b[0x1f] & 0x80)) ? "yes" : "NO");

	libusb_close(h);
	libusb_exit(ctx);
	return 0;
}
