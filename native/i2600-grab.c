/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Thomas Parthe */
/*
 * i2600-grab -- fetch raw images from a Kodak i2600 without any Kodak
 * library.  The device half lives in i2600-core.c; this file is the command
 * line front end: options, and writing the blocks out as PGM.
 *
 * See NATIVES-BACKEND.md for what the device does and why.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "i2600-core.h"
#include "i2600-firmware.h"
#include "sequence.h"
#include "sequence-duplex.h"

/* P5 for grey, P6 for colour -- same layout, three bytes per pixel. */
static int write_pnm(const char *path, const unsigned char *data,
		     unsigned width, unsigned lines, unsigned bpp)
{
	size_t want = (size_t)width * lines * bpp;
	FILE *fh = fopen(path, "wb");
	if (!fh) {
		fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
		return -1;
	}
	fprintf(fh, "P%c\n%u %u\n255\n", bpp == 3 ? '6' : '5', width, lines);
	if (fwrite(data, 1, want, fh) != want) {
		fprintf(stderr, "short write on %s\n", path);
		fclose(fh);
		return -1;
	}
	fclose(fh);
	return 0;
}

/*
 * Turn one block into a PNM.  rotate is set for rear-side images: the second
 * camera sees the sheet from the other side, so its image arrives upside
 * down.  For 8-bit grey that rotation is just reversing the pixel array.
 */
static int do_crop;
static int do_rotate = 1;   /* default on: sheets go in face up */

static int save_block(struct block *b, const char *path, int rotate)
{
	struct footer f = i2600_parse_footer(b->data, b->len);
	size_t image_bytes;
	unsigned width, lines, bpp;

	if (!f.ok) {
		char raw[512];
		snprintf(raw, sizeof raw, "%s.raw", path);
		fprintf(stderr, "footer did not parse; dumping raw block to %s\n", raw);
		FILE *fh = fopen(raw, "wb");
		if (fh) { fwrite(b->data, 1, b->len, fh); fclose(fh); }
		return -1;
	}
	printf("footer: %zu bytes, junk=%u format=%u type=%u width=%u px "
	       "xres=%d dpi yres=%d dpi segment=%u last=%d doccount=%u\n",
	       f.footer_bytes, f.junk_words, f.format, f.type, f.width_pixels,
	       i2600_res_to_dpi(f.xres_code), i2600_res_to_dpi(f.yres_code),
	       f.segment, f.last_segment, f.document_count);

	image_bytes = b->len - f.footer_bytes;
	width = f.width_pixels ? f.width_pixels : I2600_LINE_BYTES;
	bpp = i2600_bytes_per_pixel();
	if (image_bytes % ((size_t)width * bpp))
		fprintf(stderr, "WARNING: %zu image bytes is not a multiple of %u\n",
			image_bytes, width * bpp);
	lines = (unsigned)(image_bytes / ((size_t)width * bpp));
	printf("image: %zu bytes = %u lines of %u pixels, %u byte/pixel\n",
	       image_bytes, lines, width, bpp);
	if (!lines)
		return -1;
	(void)rotate;
	if (do_crop) {
		struct i2600_crop c;
		if (i2600_autocrop(b->data, width, lines, bpp, &c) == 0) {
			lines = i2600_apply_crop(b->data, width, bpp, &c, &width);
			printf("cropped to %u x %u\n", width, lines);
		}
	}
	if (do_rotate)
		i2600_rotate180(b->data, (size_t)width * lines * bpp, bpp);
	if (write_pnm(path, b->data, width, lines, bpp) < 0)
		return -1;
	printf("wrote %s (%u x %u, %s%s)\n", path, width, lines,
	       bpp == 3 ? "24 bit colour" : "8 bit gray",
	       do_rotate ? ", rotated 180" : "");
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
	"usage: %s [options]\n"
	"  -o, --out PREFIX    output file prefix (default: scan)\n"
	"  -s, --skip LIST     comma separated group names to skip\n"
	"  -p, --pages N       stop after N pages (default: until feeder empty)\n"
	"  -d, --duplex        also configure the rear camera and read EP 0x86\n"
	"  -c, --color         colour scan (24 bit) instead of 8 bit grey\n"
	"  -C, --crop          trim the dark transport border around the page\n"
	"  -r, --no-rotate     do not turn pages upside down (default: rotate)\n"
	"  -R, --no-ref        skip the reference pass\n"
	"  -S, --no-scan       only the reference pass, no paper needed\n"
	"  -k, --keep-going    do not abort on a failed control transfer\n"
	"  -v, --verbose       -vv logs every control transfer\n"
	"  -l, --list-groups   list the skippable groups and exit\n"
	"  -F, --firmware SRC  where to take the firmware images from: the vendor's\n"
	"                      devicemanager.so, or a directory with loader.bin,\n"
	"                      fpga.bin, scanner.bin and scanner0..3.bin\n"
	"                      (default: %s)\n"
	"  -N, --no-firmware   never download firmware, fail if the device is cold\n"
	"  -f, --force-firmware  download even if the scanner firmware is running\n",
	argv0, I2600_FW_DEFAULT_SOURCE);
}

static int parse_skip(const char *list)
{
	char *copy = strdup(list), *tok, *save;
	int bad = 0;
	for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		int i, found = 0;
		if (!strcmp(tok, "duplex")) {
			i2600_skip_mask |= 1u << GRP_DUPLEX;
			continue;
		}
		for (i = 0; i < GRP_COUNT; i++)
			if (!strcmp(tok, i2600_group_name[i])) {
				i2600_skip_mask |= 1u << i;
				found = 1;
				break;
			}
		if (!found) {
			fprintf(stderr, "unknown group: %s\n", tok);
			bad = 1;
		}
	}
	free(copy);
	return bad ? -1 : 0;
}

int main(int argc, char **argv)
{
	const char *prefix = "scan";
	const char *ext;
	int do_ref = 1, do_scan = 1, max_pages = 0;
	int rc = 1, page = 0;
	char path[512];

	static const struct option opts[] = {
		{ "out",         required_argument, NULL, 'o' },
		{ "skip",        required_argument, NULL, 's' },
		{ "pages",       required_argument, NULL, 'p' },
		{ "duplex",      no_argument,       NULL, 'd' },
		{ "color",       no_argument,       NULL, 'c' },
		{ "crop",        no_argument,       NULL, 'C' },
		{ "no-rotate",   no_argument,       NULL, 'r' },
		{ "no-ref",      no_argument,       NULL, 'R' },
		{ "no-scan",     no_argument,       NULL, 'S' },
		{ "keep-going",  no_argument,       NULL, 'k' },
		{ "verbose",     no_argument,       NULL, 'v' },
		{ "list-groups", no_argument,       NULL, 'l' },
		{ "firmware",    required_argument, NULL, 'F' },
		{ "no-firmware", no_argument,       NULL, 'N' },
		{ "force-firmware", no_argument,    NULL, 'f' },
		{ "help",        no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	int c;

	i2600_verbose = 1;
	while ((c = getopt_long(argc, argv, "o:s:p:dcCrRSkvlF:Nfh", opts, NULL)) != -1) {
		switch (c) {
		case 'o': prefix = optarg; break;
		case 's': if (parse_skip(optarg) < 0) return 2; break;
		case 'p': max_pages = atoi(optarg); break;
		case 'd': i2600_duplex = 1; break;
		case 'c': i2600_color = 1; break;
		case 'C': do_crop = 1; break;
		case 'r': do_rotate = 0; break;
		case 'R': do_ref = 0; break;
		case 'S': do_scan = 0; break;
		case 'k': i2600_keep_going = 1; break;
		case 'F': i2600_fw_source = optarg; break;
		case 'N': i2600_fw_mode = 0; break;
		case 'f': i2600_fw_mode = 2; break;
		case 'v': i2600_verbose++; break;
		case 'l': {
			int i;
			for (i = 0; i < GRP_COUNT; i++)
				printf("%s\n", i2600_group_name[i]);
			printf("duplex\n");       /* the whole --duplex sequence */
			return 0;
		}
		default: usage(argv[0]); return c == 'h' ? 0 : 2;
		}
	}

	/* P5 is .pgm, P6 is .ppm -- name the files accordingly. */
	ext = i2600_color ? "ppm" : "pgm";

	if (i2600_device_open() < 0)
		return 1;
	rc = 0;

	if (do_ref) {
		struct block f = { NULL, 0 }, r = { NULL, 0 };
		if (i2600_reference(&f, i2600_duplex ? &r : NULL) == 0) {
			if (r.data) {
				snprintf(path, sizeof path, "%s-reference-rear.%s", prefix, ext);
				save_block(&r, path, 0);
				i2600_block_free(&r);
			}
			snprintf(path, sizeof path, "%s-reference.%s", prefix, ext);
			save_block(&f, path, 0);
			i2600_block_free(&f);
		} else {
			rc = 1;
		}
	}

	if (do_scan && i2600_scan_begin() == 0) {
		for (;;) {
			struct block f = { NULL, 0 }, r = { NULL, 0 };
			int got = i2600_scan_next_page(&f, i2600_duplex ? &r : NULL);
			if (got <= 0)
				break;
			page++;
			snprintf(path, sizeof path, "%s-page%d.%s", prefix, page, ext);
			save_block(&f, path, 0);
			i2600_block_free(&f);
			if (r.data) {
				snprintf(path, sizeof path, "%s-page%d-rear.%s", prefix, page, ext);
				save_block(&r, path, 0);
				i2600_block_free(&r);
			}
			if (max_pages && page >= max_pages)
				break;
		}
		i2600_scan_end(max_pages && page >= max_pages);
		if (!page)
			rc = 1;
	} else if (do_scan) {
		rc = 1;
	}

	i2600_device_close();
	return rc;
}
