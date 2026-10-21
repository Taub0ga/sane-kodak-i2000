/*
 * i2600-pdf -- write the scanned pages of one batch into a single PDF.
 *
 * paperless-ngx consumes PDFs, so a document scanner should hand it one file
 * per batch rather than a pile of PGMs.  Pages go in as JPEG and are
 * referenced with /DCTDecode, which keeps the writer small (no zlib stream
 * bookkeeping) and the files roughly a tenth of the raw size.  300 dpi, the
 * only resolution the device has, becomes the page size in points.
 */
#ifndef I2600_PDF_H
#define I2600_PDF_H

#include <stdio.h>

#define I2600_PDF_MAX_PAGES 512

struct i2600_pdf {
	FILE    *fh;
	char    *path;
	long     offset[3 * I2600_PDF_MAX_PAGES + 2];  /* xref, by object - 1 */
	unsigned objects;
	unsigned pages;
	unsigned page_obj[I2600_PDF_MAX_PAGES];
	int      failed;
};

/* Open PATH for writing and emit the header. */
int i2600_pdf_begin(struct i2600_pdf *p, const char *path);

/*
 * Append one page.  IMG is WIDTH x LINES pixels with BPP bytes each (1 grey,
 * 3 RGB), DPI is the scan resolution and QUALITY the JPEG quality 1..100.
 */
int i2600_pdf_add_page(struct i2600_pdf *p, const unsigned char *img,
		       unsigned width, unsigned lines, unsigned bpp,
		       unsigned dpi, int quality);

/* Write the page tree, catalog and xref, then close.  0 on success. */
int i2600_pdf_end(struct i2600_pdf *p);

/* Give up on a half-written file and remove it. */
void i2600_pdf_abort(struct i2600_pdf *p);

#endif /* I2600_PDF_H */
