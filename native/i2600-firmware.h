/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Thomas Parthe */
/*
 * i2600-firmware -- firmware download for the Kodak i2600.
 *
 * After every cold start (mains off, USB unplugged) the scanner comes up in
 * its booter and answers nothing but GetStatus (C0 00); every other vendor
 * request stalls.  The application firmware has to be downloaded by the host
 * first.  This module rebuilds that download from
 * DeviceManager::InitializeScanner in the vendor's devicemanager.so --
 * see NATIVES-BACKEND.md section 2.
 */

#ifndef I2600_FIRMWARE_H
#define I2600_FIRMWARE_H

#include <stdint.h>
#include <stddef.h>

#include <libusb-1.0/libusb.h>

/* Byte 0 of the 32-byte status block, named by
 * InitializeScanner::IsFirmwareComponentRunning ("Component: %d  Version: %08X"). */
#define I2600_COMPONENT_BOOTER   1
#define I2600_COMPONENT_LOADER   2
#define I2600_COMPONENT_SCANNER  3

/* The six images the download needs, in the order LoadBankedFirmware asks for
 * them.  Resource name in devicemanager.so / file name of the vendor's
 * optional on-disk override. */
enum i2600_fw_image {
	FW_LOADER,       /* LOADER   / loader.bin   16 KiB, FX2 internal RAM  */
	FW_FPGA,         /* FPGA     / fpga.bin     Altera bitstream          */
	FW_SCANNER,      /* SCANNER  / scanner.bin  16 KiB, FX2 internal RAM  */
	FW_SCANNER0,     /* SCANNER0 / scanner0.bin 32 KiB, code bank 0       */
	FW_SCANNER1,
	FW_SCANNER2,
	FW_SCANNER3,
	FW_IMAGE_COUNT
};

struct i2600_firmware {
	unsigned char *image[FW_IMAGE_COUNT];
	uint32_t       len[FW_IMAGE_COUNT];
	char           source[4096];   /* where the images came from, for logging */
};

/* Default place to look for the images: the vendor driver's device manager,
 * which carries them as named resources (see i2600_fw_load). */
#define I2600_FW_DEFAULT_SOURCE "/opt/kodak/kds_i2000/lib/devicemanager.so"

/*
 * Load the six images.
 *
 * `source` may be
 *   - a file: parsed as a Kodak module with an appended "@!@kds@!@" resource
 *     directory (devicemanager.so), or
 *   - a directory: the images are read from loader.bin, fpga.bin, scanner.bin
 *     and scanner0.bin .. scanner3.bin in it, which is the vendor's own file
 *     fallback in InitializeScanner::InitializeBufferWithFile.
 *
 * Returns 0 on success.  On failure nothing is allocated and a message has
 * been written to stderr.
 */
int  i2600_fw_load(struct i2600_firmware *fw, const char *source);
void i2600_fw_free(struct i2600_firmware *fw);

/* Byte 0 of GetStatus, or -1 if even GetStatus fails. */
int  i2600_fw_component(libusb_device_handle *h);

/*
 * Run the whole download: LoadLoader, ProgramFpga, LoadBankedFirmware.
 * Expects the device in component 1 (booter) or 2 (loader); returns 0 once
 * the device reports component 3 (scanner).
 *
 * `log` is called with printf semantics for progress and errors and may be
 * NULL.  `verbose` != 0 logs every single chunk.
 */
int  i2600_fw_download(libusb_device_handle *h, const struct i2600_firmware *fw,
		       void (*log)(const char *fmt, ...), int verbose);

#endif /* I2600_FIRMWARE_H */
