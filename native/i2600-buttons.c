/*
 * i2600-buttons -- listen on the operator panel and print what arrives.
 *
 * The embedded pipe configuration flags EP 0x81 with
 * OS_USBPIPEFLAGS_MONITORBUTTONEVENTS and EP 0x88 with
 * ...NOTMONITORBUTTONEVENTS, so button presses should show up on 0x81.
 * Capturing the vendor driver did not help here: its SANE backend never
 * polls while the device merely sits open, so nothing was there to see.
 * This asks the device directly instead.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "i2600-core.h"

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 30.0;
	unsigned wvalue = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 0x0009;
	unsigned windex = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 0x01ff;
	int intr_index  = argc > 4 ? (int)strtol(argv[4], NULL, 0) : -1;
	unsigned char b[8];
	double t0;
	int got = 0;
	unsigned seen;

	i2600_verbose = 1;
	if (i2600_device_open() < 0)
		return 1;

	/* wValue 0xffff means: do not touch the panel at all.  The value the
	 * vendor used (9) is written to the panel's display, so sending it
	 * also fixes the number the user sees. */
	if (wvalue == 0xffff) {
		printf("not touching the panel (no SetButtonSequence)\n");
	} else {
		printf("arming the panel: SetButtonSequence wValue=0x%04x wIndex=0x%04x\n",
		       wvalue, windex);
		if (i2600_buttons_enable((uint16_t)wvalue, (uint16_t)windex) < 0)
			printf("  (request failed -- listening anyway)\n");
	}

	/* The open phase already sent StartInterruptEvents with wIndex=1.
	 * Which events that selects is not documented anywhere we have, so
	 * allow trying other values from the command line. */
	if (intr_index >= 0) {
		printf("StartInterruptEvents wValue=0x0001 wIndex=0x%04x\n",
		       (unsigned)intr_index);
		if (i2600_interrupt_events(1, (uint16_t)intr_index) < 0)
			printf("  (request failed)\n");
	}

	printf("listening for %.0f s -- press the panel buttons now\n"
	       "(button presses arrive on EP 0x88 as event 0x20)\n", secs);
	t0 = now_s();
	seen = i2600_event_count(I2600_EV_BUTTON_PRESS);
	while (now_s() - t0 < secs) {
		unsigned now = i2600_event_count(I2600_EV_BUTTON_PRESS);
		int n = i2600_read_button(b, 200);   /* EP 0x81, if it ever speaks */

		if (n > 0) {
			printf("[%6.2f] EP81 %d bytes:", now_s() - t0, n);
			for (int i = 0; i < n; i++)
				printf(" %02x", b[i]);
			putchar('\n');
		}
		if (now != seen) {
			unsigned char st[32];
			seen = now;
			got++;
			printf("[%6.2f] button press #%u, event value1=%u\n",
			       now_s() - t0, now, i2600_event_value1(I2600_EV_BUTTON_PRESS));
			if (i2600_get_status(st) >= 0) {
				printf("           status:");
				for (int i = 0; i < 32; i++)
					printf(" %02x", st[i]);
				putchar('\n');
			}
			fflush(stdout);
		}
	}
	printf("done: %d message(s) on the button channel\n", got);
	i2600_device_close();
	return 0;
}
