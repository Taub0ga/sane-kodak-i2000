#!/bin/bash
# Capture the USB traffic of one Kodak scanner operation with usbmon.
#
#   sudo tools/kodak-capture.sh <label> <command...>
#
# Writes <outdir>/<label>.usbmon (raw usbmon "1u" text) and runs <command>
# as the user who invoked sudo.  Set OUTDIR to choose the directory, BUSNUM
# to capture a bus other than the one the scanner is on.
set -u

LABEL=${1:?usage: kodak-capture.sh <label> <command...>}
shift
OUTDIR=${OUTDIR:-$PWD/captures}
RUNAS=${SUDO_USER:-root}

if [ "$(id -u)" != 0 ]; then
  echo "must run as root (usbmon lives in debugfs)" >&2
  exit 1
fi

# Locate the scanner if the bus was not given explicitly.
if [ -z "${BUSNUM:-}" ]; then
  line=$(lsusb -d 040a:601d | head -1)
  if [ -z "$line" ]; then
    echo "no Kodak i2600 (040a:601d) found on the USB bus" >&2
    exit 1
  fi
  BUSNUM=$(echo "$line" | sed -E 's/^Bus ([0-9]+).*/\1/')
  DEVNUM=$(echo "$line" | sed -E 's/^Bus [0-9]+ Device ([0-9]+).*/\1/')
else
  DEVNUM=${DEVNUM:-unknown}
fi
BUS=$((10#$BUSNUM))

modprobe usbmon || exit 1
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
MON=/sys/kernel/debug/usb/usbmon/${BUS}u
[ -r "$MON" ] || { echo "no $MON" >&2; exit 1; }

mkdir -p "$OUTDIR"
OUT=$OUTDIR/$LABEL.usbmon
: > "$OUT"

cat "$MON" > "$OUT" &
CATPID=$!
sleep 0.5

{
  echo "# label:   $LABEL"
  echo "# date:    $(date -Is)"
  echo "# bus:     $BUS  device: $DEVNUM  (040a:601d)"
  echo "# command: $*"
  echo "# user:    $RUNAS"
} > "$OUTDIR/$LABEL.meta"

echo "=== capturing bus $BUS -> $OUT ==="
if [ "$RUNAS" != root ]; then
  runuser -u "$RUNAS" -- "$@"
else
  "$@"
fi
RC=$?
echo "=== command exited with $RC ==="

sleep 1
kill "$CATPID" 2>/dev/null
wait "$CATPID" 2>/dev/null

echo "rc=$RC" >> "$OUTDIR/$LABEL.meta"
chown "${SUDO_UID:-0}:${SUDO_GID:-0}" "$OUT" "$OUTDIR/$LABEL.meta" 2>/dev/null
echo "=== $(wc -l < "$OUT") usbmon events in $OUT ==="
exit $RC
