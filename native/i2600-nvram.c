#include <stdlib.h>
#include <stdint.h>
/*
 * i2600-nvram -- read the calibration block from NvRam (vendor request
 * 0x35 IN, wValue 0, 128 bytes) and show it as u16 fields.
 *
 * The vendor's dump strings name u16FrontLineWidth, u16RearLineWidth,
 * u16Front/RearLeftPixel, u16Front/RearCameraDistance and
 * u16MagnificationTargetFixedLength -- see INITIALISIERUNG.md section 8.
 * The field order is not documented, so this prints every offset and flags
 * the values that INITIALISIERUNG.md already identified, which pins the rest.
 */
#include <stdio.h>
#include <libusb.h>

#define NVRAM_LEN 128

static const struct { unsigned v; const char *name; } known[] = {
	{ 3126, "MagnificationTargetFixedLength" },
	{ 3122, "MagnificationTargetCalibrationLength" },
	{  901, "Front LE/TE offset" },
	{  373, "Rear LE/TE offset" },
	{ 2580, "line width in pixels (measured geometry)" },
};

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	unsigned char b[NVRAM_LEN] = { 0 };
	int r, i, k;
	unsigned wvalue = (argc > 1) ? (unsigned)strtoul(argv[1], NULL, 0) : 0;

	if (libusb_init(&ctx) < 0)
		return 1;
	h = libusb_open_device_with_vid_pid(ctx, 0x040a, 0x601d);
	if (!h) { fprintf(stderr, "no Kodak i2600 found\n"); return 1; }

	r = libusb_control_transfer(h, 0xC0, 0x35, (uint16_t)wvalue, 0,
				    b, sizeof b, 3000);
	if (r < 0) {
		fprintf(stderr, "GetNvRamData(wValue=%u): %s\n",
			wvalue, libusb_error_name(r));
		libusb_close(h); libusb_exit(ctx); return 1;
	}
	printf("GetNvRamData wValue=%u -> %d bytes\n\n", wvalue, r);
	for (i = 0; i < r; i++) {
		if (i % 16 == 0) printf("  +%02x  ", i);
		printf("%02x ", b[i]);
		if (i % 16 == 15) putchar('\n');
	}
	if (r % 16) putchar('\n');

	/* The numeric fields sit at odd offsets -- the block opens with an
	 * ASCII calibration timestamp of 24 bytes including its NUL, so a
	 * naive even-offset reading misses every field.  Scan every byte. */
	printf("\n  offset   u16 LE   u16 BE   note\n");
	for (i = 0; i + 1 < r; i += 1) {
		unsigned le = (unsigned)b[i] | ((unsigned)b[i+1] << 8);
		unsigned be = ((unsigned)b[i] << 8) | (unsigned)b[i+1];
		const char *note = "";
		for (k = 0; k < (int)(sizeof known / sizeof known[0]); k++)
			if (le == known[k].v || be == known[k].v)
				note = known[k].name;
		if (*note || (le && le < 60000 && i >= 0x18))
			printf("   +0x%02x   %6u   %6u   %s\n", i, le, be, note);
	}
	libusb_close(h);
	libusb_exit(ctx);
	return 0;
}
