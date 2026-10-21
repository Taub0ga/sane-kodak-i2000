#!/bin/bash
# Root-side helper for usbmon captures.
#
#   sudo tools/usbmon-daemon.sh <ctrl-dir>
#
# usbmon's text interface lives in debugfs, which is root-only, while the scan
# itself has to run as the desktop user.  Rather than asking for a password for
# every single capture, this script is started once as root and then driven
# through small control files in <ctrl-dir>:
#
#   echo "START <label>" > <ctrl-dir>/cmd   begin capturing into <label>.usbmon
#   echo "STOP"          > <ctrl-dir>/cmd   stop the running capture
#   echo "QUIT"          > <ctrl-dir>/cmd   exit the daemon
#
# It writes <ctrl-dir>/state (idle|capturing <label>) after each command and
# chowns the captures to the invoking user.  Labels are restricted to
# [A-Za-z0-9._-] so nothing can escape <ctrl-dir>.
#
# NOTE on <ctrl-dir> when tshark is used: dumpcap, which tshark spawns to do
# the actual writing, drops all capabilities including CAP_DAC_OVERRIDE as soon
# as it has opened the capture device.  A uid-0 process without that capability
# is subject to the normal permission checks, so it cannot traverse a
# drwx------ directory owned by somebody else - it fails with
#   tshark: The file to which the capture would be saved (...) could not be
#   opened: Permission denied.
# Pick a <ctrl-dir> whose whole path is traversable by others, for instance
# one under /tmp.  The text fallback below is written by this script itself,
# which keeps its capabilities, and works anywhere.
#
# If tshark is installed it captures to <label>.pcapng through usbmon's binary
# interface, which records whole payloads.  Otherwise it falls back to the
# debugfs text interface (<label>.usbmon), which truncates every transfer's
# data to 32 bytes - enough to identify requests, not enough to read a 120-byte
# scan setup block.  Set SNAPLEN to change the per-transfer capture limit
# (default 4096: full control transfers, truncated image data).
set -u

CTRL=${1:?usage: usbmon-daemon.sh <ctrl-dir>}
OWNER_UID=${SUDO_UID:-0}
OWNER_GID=${SUDO_GID:-0}

[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 1; }

modprobe usbmon || exit 1
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug

TSHARK=$(command -v tshark || true)
SNAPLEN=${SNAPLEN:-4096}
if [ -n "$TSHARK" ]; then
  echo "using tshark: full payloads up to $SNAPLEN bytes, output .pcapng"
else
  echo "tshark not found: falling back to the debugfs text interface," \
       "which truncates payloads to 32 bytes"
fi

mkdir -p "$CTRL"
: > "$CTRL/cmd"
chown "$OWNER_UID:$OWNER_GID" "$CTRL/cmd"
CATPID=""
LABEL=""
OUTFILE=""

say() { echo "$*"; echo "$*" > "$CTRL/state"; chown "$OWNER_UID:$OWNER_GID" "$CTRL/state"; }

stop_capture() {
  [ -n "$CATPID" ] || return 0
  sleep 1                       # let the tail of the transfer arrive
  kill "$CATPID" 2>/dev/null
  wait "$CATPID" 2>/dev/null
  chown "$OWNER_UID:$OWNER_GID" "$CTRL/$OUTFILE" 2>/dev/null
  if [ "${OUTFILE##*.}" = pcapng ]; then
    if [ -s "$CTRL/$OUTFILE" ]; then
      say "idle (wrote $OUTFILE, $(du -h "$CTRL/$OUTFILE" | cut -f1))"
    else
      say "error: $OUTFILE empty or missing; see $LABEL.tshark.log"
    fi
  else
    say "idle (wrote $OUTFILE, $(wc -l < "$CTRL/$OUTFILE") events)"
  fi
  CATPID=""; LABEL=""; OUTFILE=""
}

say "idle"
echo "usbmon daemon ready; control dir $CTRL. Ctrl-C or 'QUIT' to stop."

while :; do
  read -r cmd arg < "$CTRL/cmd" 2>/dev/null || true
  case "$cmd" in
    START)
      case "$arg" in
        ''|*[!A-Za-z0-9._-]*) say "error: bad label"; : > "$CTRL/cmd"; continue ;;
      esac
      stop_capture
      bus=$(lsusb -d 040a:601d | sed -E 's/^Bus 0*([0-9]+).*/\1/' | head -1)
      if [ -z "$bus" ]; then say "error: scanner 040a:601d not on the bus"; : > "$CTRL/cmd"; continue; fi
      LABEL=$arg
      if [ -n "$TSHARK" ]; then
        if [ ! -e "/dev/usbmon$bus" ]; then
          say "error: /dev/usbmon$bus missing"; : > "$CTRL/cmd"; continue
        fi
        OUTFILE=$LABEL.pcapng
        "$TSHARK" -i "usbmon$bus" -s "$SNAPLEN" -q \
                  -w "$CTRL/$OUTFILE" > "$CTRL/$LABEL.tshark.log" 2>&1 &
        CATPID=$!
        sleep 1.5
      else
        mon=/sys/kernel/debug/usb/usbmon/${bus}u
        if [ ! -r "$mon" ]; then
          say "error: $mon not readable"; : > "$CTRL/cmd"; continue
        fi
        OUTFILE=$LABEL.usbmon
        cat "$mon" > "$CTRL/$OUTFILE" &
        CATPID=$!
        sleep 0.3
      fi
      say "capturing $LABEL (bus $bus) -> $OUTFILE"
      ;;
    STOP) stop_capture ;;
    QUIT) stop_capture; say "exiting"; exit 0 ;;
    '') ;;
    *) say "error: unknown command <$cmd>" ;;
  esac
  [ -n "$cmd" ] && : > "$CTRL/cmd"
  sleep 0.2
done
