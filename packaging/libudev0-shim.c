/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Thomas Parthe */
/*
 * Forwarding shim for libudev.so.0.
 *
 * libopenusb's Linux backend (openusb_backend/linux.so) was built against the
 * pre-systemd libudev SONAME "libudev.so.0".  All 21 udev symbols it imports
 * still exist with unchanged signatures in today's libudev.so.1, and the
 * references in linux.so are unversioned, so no symbol wrapping is needed.
 *
 * This translation unit is intentionally empty: the resulting library carries
 * only SONAME=libudev.so.0 and DT_NEEDED=libudev.so.1, which makes the dynamic
 * loader satisfy linux.so's dependency from the system libudev.
 */
