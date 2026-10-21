#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
# Keep the scanner open for a while so its interrupt endpoints get polled, so
# that pressing a button on the operator panel shows up in a usbmon capture.
#
#   tools/capture-button-press.sh <ctrl-dir> <label> [seconds]
#
# Needs tools/usbmon-daemon.sh running as root on <ctrl-dir>.  scanimage is
# started with --batch --batch-prompt and its stdin connected to a fifo that is
# never written to, so it opens the device and then waits instead of pulling a
# page.
set -u
CTRL=${1:?usage: capture-button-press.sh <ctrl-dir> <label> [seconds]}
LABEL=${2:?}
SECS=${3:-20}
DEV=${DEV:-kds_i2000:i2000}

FIFO=$(mktemp -u "${TMPDIR:-/tmp}/kodak-btn.XXXXXX")
mkfifo "$FIFO" || exit 1
trap 'rm -f "$FIFO"' EXIT

echo "START $LABEL" > "$CTRL/cmd"
sleep 2
cat "$CTRL/state"

# Hold the write end open for SECS, then close it.  scanimage sees EOF on its
# prompt and shuts down on its own: it ignores SIGTERM while the backend is
# open, so killing it leaves the device locked.
( sleep "$SECS"; ) > "$FIFO" &
HOLD=$!
scanimage -d "$DEV" --batch --batch-prompt < "$FIFO" > /dev/null 2>&1 &
SCAN=$!

echo ">>> device is open for ${SECS}s - press buttons on the scanner now <<<"
wait "$HOLD" 2>/dev/null

# give it a few seconds to close the device cleanly, then insist
for _ in 1 2 3 4 5 6 7 8 9 10; do
  kill -0 "$SCAN" 2>/dev/null || break
  sleep 1
done
if kill -0 "$SCAN" 2>/dev/null; then
  kill -INT "$SCAN" 2>/dev/null; sleep 3
  kill -0 "$SCAN" 2>/dev/null && kill -9 "$SCAN" 2>/dev/null
fi
wait "$SCAN" 2>/dev/null

echo "STOP" > "$CTRL/cmd"
sleep 3
cat "$CTRL/state"
