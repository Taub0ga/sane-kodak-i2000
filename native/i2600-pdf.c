/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Thomas Parthe */
/*
 * i2600-pdf -- one PDF per scanned batch, pages embedded as JPEG.
 *
 * The file is written straight out, page by page, so a long batch never has
 * to be held in memory: every object's offset is recorded as it goes and the
 * page tree, the catalog and the cross reference table follow at the end.
 * That is why object numbers are handed out in scan order and the catalog
 * ends up with the highest one -- PDF does not care, the trailer says which
 * object is the root.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <jpeglib.h>

#include "i2600-pdf.h"

/*
 * Object 1 is reserved for the page tree, whose /Kids list is only complete
 * at the end: a page dictionary needs its /Parent right away, and a number
 * fixed up afterwards would not fit the gap it left.  So the first object
 * handed out here is 2, and offset[0] is filled in when the tree is written.
 */
#define PDF_PAGES_OBJ 1

/* Record where the next object starts and hand out its number. */
static unsigned next_object(struct i2600_pdf *p)
{
	if (p->objects >= sizeof p->offset / sizeof p->offset[0]) {
		p->failed = 1;
		return 0;
	}
	p->offset[p->objects] = ftell(p->fh);
	return ++p->objects;
}

static void emit(struct i2600_pdf *p, const char *fmt, ...)
{
	va_list ap;

	if (p->failed)
		return;
	va_start(ap, fmt);
	if (vfprintf(p->fh, fmt, ap) < 0)
		p->failed = 1;
	va_end(ap);
}

int i2600_pdf_begin(struct i2600_pdf *p, const char *path)
{
	memset(p, 0, sizeof *p);
	p->objects = PDF_PAGES_OBJ;      /* object 1 is the page tree, written last */
	p->fh = fopen(path, "wb");
	if (!p->fh)
		return -1;
	p->path = strdup(path);
	if (!p->path) {
		fclose(p->fh);
		p->fh = NULL;
		return -1;
	}
	/* The binary comment line tells transfer tools this is not text. */
	emit(p, "%%PDF-1.4\n%%\xe2\xe3\xcf\xd3\n");
	return p->failed ? -1 : 0;
}

/* Compress one page into a JPEG in memory.  Caller frees *out. */
static int jpeg_compress(const unsigned char *img, unsigned width,
			 unsigned lines, unsigned bpp, int quality,
			 unsigned char **out, unsigned long *out_len)
{
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	unsigned y;

	*out = NULL;
	*out_len = 0;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	jpeg_mem_dest(&cinfo, out, out_len);
	cinfo.image_width      = width;
	cinfo.image_height     = lines;
	cinfo.input_components = (int)bpp;
	cinfo.in_color_space   = bpp == 3 ? JCS_RGB : JCS_GRAYSCALE;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);
	for (y = 0; y < lines; y++) {
		JSAMPROW row = (JSAMPROW)(img + (size_t)y * width * bpp);
		jpeg_write_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	if (!*out || !*out_len) {
		free(*out);
		*out = NULL;
		return -1;
	}
	return 0;
}

int i2600_pdf_add_page(struct i2600_pdf *p, const unsigned char *img,
		       unsigned width, unsigned lines, unsigned bpp,
		       unsigned dpi, int quality)
{
	unsigned char *jpg = NULL;
	unsigned long jpg_len = 0;
	unsigned img_obj, content_obj, page_obj;
	double w_pt, h_pt;
	char content[128];
	int content_len;

	if (p->failed || !p->fh)
		return -1;
	if (p->pages >= I2600_PDF_MAX_PAGES) {
		p->failed = 1;
		return -1;
	}
	if (!width || !lines || (bpp != 1 && bpp != 3) || !dpi) {
		p->failed = 1;
		return -1;
	}
	if (jpeg_compress(img, width, lines, bpp, quality, &jpg, &jpg_len) < 0) {
		p->failed = 1;
		return -1;
	}

	/* 72 points to the inch: the pixel grid becomes a physical page. */
	w_pt = (double)width * 72.0 / dpi;
	h_pt = (double)lines * 72.0 / dpi;

	img_obj = next_object(p);
	emit(p, "%u 0 obj\n<< /Type /XObject /Subtype /Image /Width %u /Height %u"
		" /ColorSpace /Device%s /BitsPerComponent 8 /Filter /DCTDecode"
		" /Length %lu >>\nstream\n",
	     img_obj, width, lines, bpp == 3 ? "RGB" : "Gray", jpg_len);
	if (!p->failed && fwrite(jpg, 1, jpg_len, p->fh) != jpg_len)
		p->failed = 1;
	free(jpg);
	emit(p, "\nendstream\nendobj\n");

	/* The image is drawn across the whole page; the matrix does the scaling. */
	content_len = snprintf(content, sizeof content,
			       "q\n%.4f 0 0 %.4f 0 0 cm\n/Im0 Do\nQ\n", w_pt, h_pt);
	content_obj = next_object(p);
	emit(p, "%u 0 obj\n<< /Length %d >>\nstream\n%sendstream\nendobj\n",
	     content_obj, content_len, content);

	page_obj = next_object(p);
	emit(p, "%u 0 obj\n<< /Type /Page /Parent %u 0 R /MediaBox [0 0 %.4f %.4f]"
		" /Resources << /XObject << /Im0 %u 0 R >> >> /Contents %u 0 R"
		" >>\nendobj\n",
	     page_obj, (unsigned)PDF_PAGES_OBJ, w_pt, h_pt, img_obj, content_obj);
	p->page_obj[p->pages++] = page_obj;
	return p->failed ? -1 : 0;
}

int i2600_pdf_end(struct i2600_pdf *p)
{
	unsigned catalog_obj, i;
	long xref;

	if (!p->fh)
		return -1;
	if (p->failed || !p->pages) {
		i2600_pdf_abort(p);
		return -1;
	}

	/* The page tree, into the number the pages were already pointed at. */
	p->offset[PDF_PAGES_OBJ - 1] = ftell(p->fh);
	emit(p, "%u 0 obj\n<< /Type /Pages /Count %u /Kids [",
	     (unsigned)PDF_PAGES_OBJ, p->pages);
	for (i = 0; i < p->pages; i++)
		emit(p, "%s%u 0 R", i ? " " : "", p->page_obj[i]);
	emit(p, "] >>\nendobj\n");

	catalog_obj = next_object(p);
	emit(p, "%u 0 obj\n<< /Type /Catalog /Pages %u 0 R >>\nendobj\n",
	     catalog_obj, (unsigned)PDF_PAGES_OBJ);

	xref = ftell(p->fh);
	emit(p, "xref\n0 %u\n0000000000 65535 f \n", p->objects + 1);
	for (i = 0; i < p->objects; i++)
		emit(p, "%010ld 00000 n \n", p->offset[i]);
	emit(p, "trailer\n<< /Size %u /Root %u 0 R >>\nstartxref\n%ld\n%%%%EOF\n",
	     p->objects + 1, catalog_obj, xref);

	if (fclose(p->fh) != 0)
		p->failed = 1;
	p->fh = NULL;
	if (p->failed) {
		if (p->path)
			unlink(p->path);
		free(p->path);
		p->path = NULL;
		return -1;
	}
	free(p->path);
	p->path = NULL;
	return 0;
}

void i2600_pdf_abort(struct i2600_pdf *p)
{
	if (p->fh) {
		fclose(p->fh);
		p->fh = NULL;
	}
	if (p->path) {
		unlink(p->path);
		free(p->path);
		p->path = NULL;
	}
	p->failed = 1;
}
