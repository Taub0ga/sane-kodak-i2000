/*
 * i2600-power -- read or set the scanner's power state.
 *
 * FCT::EnterEnergyStar() in the vendor's devicemanager.so runs
 * ScannerOperation 0x16, which is bRequest 0x39 wValue 1; ExitEnergyStar()
 * runs 0x17, wValue 2.  Operation 0x15 is the same request with wValue 0.
 * Scanner::GetPowerState() reads the status block and returns byte 0x0c.
 * See NATIVES-BACKEND.md section 15.
 *
 * Like i2600-status this talks to the device directly instead of going
 * through i2600-core: the core's open sequence is 271 control transfers and
 * would wake a sleeping scanner before we could look at it.
 */
#include <stdio.h>
#include <string.h>
#include <libusb.h>

#define REQ_STATUS 0x00
#define REQ_POWER  0x39

static const char *state_name(unsigned char s)
{
	switch (s) {
	case 1:  return "reduced -- energy saver";
	case 3:  return "idle, ready";
	case 4:  return "scanning";
	default: return "unknown";
	}
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	unsigned char b[32] = { 0 };
	int r, set = -1;

	if (argc > 1) {
		if (!strcmp(argv[1], "saver"))       set = 1;
		else if (!strcmp(argv[1], "on"))     set = 2;
		else if (!strcmp(argv[1], "normal")) set = 0;
		else {
			fprintf(stderr,
				"usage: %s [saver|on|normal]\n"
				"  no argument  report the power state (status byte 0x0c)\n"
				"  saver        enter the energy saver (0x%02x wValue 1)\n"
				"  on           leave it again        (0x%02x wValue 2)\n"
				"  normal       operation 0x15        (0x%02x wValue 0)\n",
				argv[0], REQ_POWER, REQ_POWER, REQ_POWER);
			return 2;
		}
	}

	if (libusb_init(&ctx) < 0)
		return 1;
	h = libusb_open_device_with_vid_pid(ctx, 0x040a, 0x601d);
	if (!h) {
		fprintf(stderr, "no Kodak i2600 found\n");
		libusb_exit(ctx);
		return 1;
	}

	if (set >= 0) {
		r = libusb_control_transfer(h, 0x40, REQ_POWER,
					    (uint16_t)set, 0, NULL, 0, 2000);
		if (r < 0) {
			fprintf(stderr, "request 0x%02x wValue %d: %s\n",
				REQ_POWER, set, libusb_error_name(r));
			libusb_close(h);
			libusb_exit(ctx);
			return 1;
		}
		printf("sent request 0x%02x wValue %d\n", REQ_POWER, set);
	}

	r = libusb_control_transfer(h, 0xC0, REQ_STATUS, 0, 0, b, sizeof b, 2000);
	if (r < 0)
		fprintf(stderr, "GetStatus: %s\n", libusb_error_name(r));
	else
		printf("power state [0x0c] = %u   (%s)\n", b[0x0c],
		       state_name(b[0x0c]));

	libusb_close(h);
	libusb_exit(ctx);
	return r < 0 ? 1 : 0;
}
