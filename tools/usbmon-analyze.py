#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
"""Summarise a usbmon capture of the Kodak i2600.

Reads either usbmon's debugfs text format (.usbmon), whose payloads are
truncated to 32 bytes, or a pcapng/pcap file written from usbmon's binary
interface (tshark -i usbmonN), which carries whole payloads.  pcapng files are
decoded by shelling out to tshark.

Usage:  usbmon-analyze.py [options] capture.usbmon|capture.pcapng

  --device N         only this USB device address (default: auto-detect the
                     device that has bulk endpoints)
  --max-cmds N       stop after N bulk-OUT commands (0 = all)
  --payload-bytes N  hex bytes to print per transfer (default 64)
  --no-stream        skip the per-command listing
  --timeline         print every transfer in chronological order instead
  --collapse-bulk    in --timeline, fold runs of image-data reads into one line

Prints:
  1. a per-endpoint traffic summary,
  2. the control transfers,
  3. the bulk OUT command stream, each command's hex bytes and the bulk IN
     traffic that arrived before the next command,
  4. histograms of the OUT commands' first bytes and lengths.

usbmon text line layout (see Documentation/usb/usbmon.rst):
  <urb-tag> <timestamp-us> <S|C|E> <type><dir>:<bus>:<dev>:<ep> \
      [s <bmRequestType> <bRequest> <wValue> <wIndex> <wLength> | <status>] \
      <length> [= <data words...> | <]
A submission of a control transfer carries the 8-byte setup packet where
other transfers carry the status; "<" means usbmon did not record the data.
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys

LINE = re.compile(
    r'^(?P<tag>\S+)\s+(?P<ts>\d+)\s+(?P<ev>[SCE])\s+'
    r'(?P<type>[CZIB])(?P<dir>[io]):(?P<bus>\d+):(?P<dev>\d+):(?P<ep>\d+)\s*'
    r'(?P<rest>.*)$')

TYPE_NAME = {'C': 'ctrl', 'Z': 'isoc', 'I': 'intr', 'B': 'bulk'}
# Endpoint roles taken from the device's own USB descriptors and the driver's
# symbol names; the direction bit is implied by usbmon's i/o.
EP_ROLE = {
    (0, 'i'): 'EP0  CONTROL in',
    (0, 'o'): 'EP0  CONTROL out',
    (2, 'o'): 'EP0x02 BULK OUT  (commands)',
    (2, 'i'): 'EP0x82 BULK IN   (front image / cmd replies)',
    (6, 'i'): 'EP0x86 BULK IN   (rear image)',
    (1, 'i'): 'EP0x81 INTR IN',
    (8, 'i'): 'EP0x88 INTR IN',
}


class Event:
    __slots__ = ('tag', 'ts', 'ev', 'type', 'dir', 'bus', 'dev', 'ep',
                 'setup', 'status', 'length', 'data', 'truncated')


def parse(path):
    events = []
    with open(path, 'r', errors='replace') as fh:
        for raw in fh:
            m = LINE.match(raw.rstrip('\n'))
            if not m:
                continue
            e = Event()
            e.tag = m.group('tag')
            e.ts = int(m.group('ts'))
            e.ev = m.group('ev')
            e.type = m.group('type')
            e.dir = m.group('dir')
            e.bus = int(m.group('bus'))
            e.dev = int(m.group('dev'))
            e.ep = int(m.group('ep'))
            e.setup = None
            e.status = ''
            e.length = 0
            e.data = b''
            e.truncated = False

            rest = m.group('rest').split()
            if rest and rest[0] == 's':
                # control submission: setup packet instead of a status
                e.setup = rest[1:6]
                rest = rest[6:]
            elif rest:
                # "status" or "status:interval" (interrupt/isochronous)
                e.status = rest[0].split(':')[0]
                rest = rest[1:]
            if rest and rest[0].lstrip('-').isdigit():
                e.length = int(rest[0])
                rest = rest[1:]
            if rest:
                if rest[0] == '=':
                    try:
                        e.data = bytes.fromhex(''.join(rest[1:]))
                    except ValueError:
                        e.data = b''
                    e.truncated = len(e.data) < e.length
                else:                       # "<" : data not recorded
                    e.truncated = e.length > 0
            events.append(e)
    return events


# tshark field list; order matters, it is unpacked positionally below.
TSHARK_FIELDS = [
    'frame.number', 'frame.time_epoch', 'usb.urb_type', 'usb.transfer_type',
    'usb.endpoint_address.number', 'usb.endpoint_address.direction',
    'usb.device_address', 'usb.urb_status', 'usb.data_len', 'usb.urb_len',
    'usb.bmRequestType', 'usb.setup.bRequest', 'usb.setup.wValue',
    'usb.setup.wIndex', 'usb.setup.wLength',
    # the payload lives in a different field depending on the transfer:
    # bulk/interrupt -> capdata, control OUT -> data_fragment,
    # control IN completion -> control.Response
    'usb.capdata', 'usb.data_fragment', 'usb.control.Response',
    'usb.getDescriptor.Response',
]
# usb.transfer_type values -> our single-letter type codes
XFER_TYPE = {'0x00': 'Z', '0x01': 'I', '0x02': 'C', '0x03': 'B',
             '0': 'Z', '1': 'I', '2': 'C', '3': 'B'}


def parse_pcapng(path):
    """Decode a pcapng/pcap capture by running tshark."""
    tshark = shutil.which('tshark')
    if not tshark:
        print(f'{path} needs tshark to decode; install wireshark-cli',
              file=sys.stderr)
        sys.exit(2)
    cmd = [tshark, '-r', path, '-T', 'fields', '-E', 'separator=\t']
    for f in TSHARK_FIELDS:
        cmd += ['-e', f]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr.strip(), file=sys.stderr)
        sys.exit(2)

    events = []
    t_first = None
    for raw in out.stdout.splitlines():
        col = raw.split('\t')
        if len(col) < len(TSHARK_FIELDS):
            col += [''] * (len(TSHARK_FIELDS) - len(col))
        (_fnum, tepoch, urb_type, xfer, epnum, epdir, devaddr, status,
         data_len, urb_len, bm, br, wval, widx, wlen,
         capdata, fragment, ctrl_resp, desc_resp) = col[:19]
        if xfer not in XFER_TYPE:
            continue
        try:
            ts = float(tepoch)
        except ValueError:
            continue
        if t_first is None:
            t_first = ts

        e = Event()
        e.ts = int(round((ts - t_first) * 1e6))
        e.ev = 'S' if urb_type.strip("'\"") == 'S' else 'C'
        e.type = XFER_TYPE[xfer]
        e.dir = 'i' if epdir == '1' else 'o'
        e.bus = 0
        e.dev = int(devaddr) if devaddr.isdigit() else -1
        e.ep = int(epnum) if epnum.isdigit() else -1
        e.tag = f'{e.dev}:{e.ep}'
        try:
            e.status = str(int(status, 0))
        except ValueError:
            e.status = ''

        e.setup = None
        if bm and br:
            def h(v, w):
                return f'{int(v, 0) & ((1 << (4 * w)) - 1):0{w}x}'
            try:
                e.setup = [h(bm, 2), h(br, 2),
                           h(wval or '0', 4), h(widx or '0', 4),
                           h(wlen or '0', 4)]
            except ValueError:
                e.setup = None

        e.data = b''
        for field in (capdata, fragment, ctrl_resp, desc_resp):
            if field:
                try:
                    e.data = bytes.fromhex(field.replace(':', ''))
                except ValueError:
                    e.data = b''
                break

        # usb.data_len is only what the capture kept (tshark -s clips it);
        # usb.urb_len is the transfer's real size.  Control transfers report
        # neither on the frame carrying the setup packet - there it is wLength.
        try:
            e.length = int(urb_len) if urb_len else 0
        except ValueError:
            e.length = 0
        if not e.length:
            try:
                e.length = int(data_len) if data_len else 0
            except ValueError:
                e.length = 0
        if e.type == 'C' and not e.length and e.setup:
            e.length = int(e.setup[4], 16)
        if not e.length:
            e.length = len(e.data)

        e.truncated = len(e.data) < e.length
        events.append(e)
    return events


def load(path):
    if path.lower().endswith(('.pcapng', '.pcap', '.pcapng.gz', '.pcap.gz')):
        return parse_pcapng(path)
    return parse(path)


def pick_device(events):
    """Guess which device address is the scanner.

    The Kodak is driven almost entirely through vendor-specific control
    requests, which is far more distinctive than "has bulk endpoints" - other
    devices on the same bus (a Bluetooth adapter, for instance) have bulk
    endpoints too.  Fall back to bulk traffic, then to the busiest device.
    """
    vendor = collections.Counter(
        e.dev for e in events
        if e.setup and int(e.setup[0], 16) & 0x60 == 0x40)
    if vendor:
        return vendor.most_common(1)[0][0]
    bulk = collections.Counter(e.dev for e in events if e.type == 'B')
    if bulk:
        return bulk.most_common(1)[0][0]
    allc = collections.Counter(e.dev for e in events)
    return allc.most_common(1)[0][0] if allc else None


def hexdump(data, limit, indent='      '):
    out = []
    shown = data[:limit]
    for off in range(0, len(shown), 16):
        chunk = shown[off:off + 16]
        hx = ' '.join(f'{b:02x}' for b in chunk)
        txt = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        out.append(f'{indent}{off:04x}  {hx:<47}  |{txt}|')
    if len(data) > limit:
        out.append(f'{indent}... {len(data) - limit} more bytes')
    return '\n'.join(out)


def role(ep, dr, typ):
    return EP_ROLE.get((ep, dr), f'{TYPE_NAME[typ]} ep{ep} {dr}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--device', type=int, default=None)
    ap.add_argument('--max-cmds', type=int, default=0)
    ap.add_argument('--payload-bytes', type=int, default=64)
    ap.add_argument('--no-stream', action='store_true')
    ap.add_argument('--timeline', action='store_true')
    ap.add_argument('--collapse-bulk', action='store_true')
    args = ap.parse_args()

    allev = load(args.capture)
    if not allev:
        print('no usbmon events parsed', file=sys.stderr)
        return 1
    dev = args.device if args.device is not None else pick_device(allev)
    events = [e for e in allev if e.dev == dev]
    t0 = events[0].ts

    print('=' * 78)
    print(f'{args.capture}')
    print(f'  {len(allev)} events total, {len(events)} for device address {dev}, '
          f'{(events[-1].ts - t0) / 1e6:.3f} s')
    others = collections.Counter(e.dev for e in allev if e.dev != dev)
    if others:
        print('  (ignored other devices on the bus: '
              + ', '.join(f'dev {d}: {n} events' for d, n in others.most_common())
              + ')')

    print()
    print('--- 1. endpoint summary (completions only) ---')
    print(f'{"endpoint":<44} {"xfers":>6} {"bytes":>11} {"min":>7} {"max":>8} {"err":>5}')
    stats = collections.defaultdict(lambda: [0, 0, None, 0, 0])
    for e in events:
        # Control transfers carry their size on the frame with the setup
        # packet, everything else on the completion.
        if e.type == 'C':
            if not e.setup:
                continue
        elif e.ev != 'C':
            continue
        s = stats[(e.type, e.dir, e.ep)]
        s[0] += 1
        s[1] += e.length
        s[2] = e.length if s[2] is None else min(s[2], e.length)
        s[3] = max(s[3], e.length)
        # -115 (EINPROGRESS) on a submission is not an error
        if e.ev == 'C' and e.status not in ('0', ''):
            s[4] += 1
    for (typ, dr, ep), s in sorted(stats.items(), key=lambda kv: (kv[0][2], kv[0][1])):
        print(f'{role(ep, dr, typ):<44} {s[0]:>6} {s[1]:>11} {s[2]:>7} '
              f'{s[3]:>8} {s[4]:>5}')

    print()
    print('--- 2. control transfers (by setup packet) ---')
    seen = collections.Counter()
    for e in events:
        if e.setup:
            seen[tuple(e.setup)] += 1
    for setup, n in seen.most_common():
        bm, br, wval, widx, wlen = setup
        print(f'  x{n:<4} bmRequestType=0x{bm} bRequest=0x{br} '
              f'wValue=0x{wval} wIndex=0x{widx} wLength={int(wlen, 16)}')
    if not seen:
        print('  (none captured)')

    if args.timeline:
        print()
        print('--- 3. chronological transfer timeline ---')
        print('    (control: direction, bRequest, wValue, wIndex, length, data)')
        pending = {}
        run = None       # collapsed run of bulk IN reads
        for e in events:
            if e.type == 'C':
                if e.ev == 'S' and e.setup:
                    pending[e.tag] = e
                    continue
                sub = pending.pop(e.tag, None)
                if sub is None:
                    continue
                bm, br, wval, widx, wlen = sub.setup
                arrow = 'IN ' if int(bm, 16) & 0x80 else 'OUT'
                kind = {0x00: 'std', 0x20: 'class', 0x40: 'vendor'}.get(
                    int(bm, 16) & 0x60, '?')
                data = e.data if arrow == 'IN ' else sub.data
                st = '' if e.status == '0' else f'  status={e.status}'
                print(f'  +{(sub.ts - t0) / 1e6:9.6f}  CTRL {arrow} {kind:<6} '
                      f'bRequest=0x{br} wValue=0x{wval} wIndex=0x{widx} '
                      f'len={int(wlen, 16)}{st}')
                if data:
                    print(hexdump(data, args.payload_bytes, '              '))
            elif e.ev == 'C':
                big = e.type == 'B' and e.dir == 'i' and e.length > 512
                if args.collapse_bulk and big:
                    if run and run[0] == e.ep:
                        run[1] += 1
                        run[2] += e.length
                        run[3] = e.ts
                        continue
                    if run:
                        print(f'  +{(run[4] - t0) / 1e6:9.6f}  {role(run[0], "i", "B")}'
                              f'  x{run[1]} transfers, {run[2]} bytes, '
                              f'until +{(run[3] - t0) / 1e6:.6f}s')
                    run = [e.ep, 1, e.length, e.ts, e.ts]
                    continue
                if run:
                    print(f'  +{(run[4] - t0) / 1e6:9.6f}  {role(run[0], "i", "B")}'
                          f'  x{run[1]} transfers, {run[2]} bytes, '
                          f'until +{(run[3] - t0) / 1e6:.6f}s')
                    run = None
                st = '' if e.status == '0' else f'  status={e.status}'
                print(f'  +{(e.ts - t0) / 1e6:9.6f}  '
                      f'{role(e.ep, e.dir, e.type)}  {e.length} bytes{st}')
                if e.data and e.length <= 512:
                    print(hexdump(e.data, args.payload_bytes, '              '))
        if run:
            print(f'  +{(run[4] - t0) / 1e6:9.6f}  {role(run[0], "i", "B")}'
                  f'  x{run[1]} transfers, {run[2]} bytes, '
                  f'until +{(run[3] - t0) / 1e6:.6f}s')

    # ---------------------------------------------------------------- stream
    stream = []
    for e in events:
        if e.type != 'B':
            continue
        if e.dir == 'o' and e.ev == 'S':      # OUT data is on the submission
            stream.append(e)
        elif e.dir == 'i' and e.ev == 'C':    # IN data is on the completion
            stream.append(e)

    if not args.no_stream and not args.timeline:
        print()
        print('--- 3. bulk OUT command stream with the bulk IN data that follows ---')
        if not stream:
            print('  (no bulk traffic captured)')
        idx = 0
        ncmd = 0
        while idx < len(stream):
            e = stream[idx]
            if e.dir != 'o':
                idx += 1
                continue
            ncmd += 1
            if args.max_cmds and ncmd > args.max_cmds:
                left = sum(1 for x in stream[idx:] if x.dir == 'o')
                print(f'\n  ... stopping after {args.max_cmds} commands ({left} more)')
                break
            print(f'\n  [{ncmd:04d}] t=+{(e.ts - t0) / 1e6:9.6f}s  OUT ep0x02  '
                  f'{e.length} bytes'
                  + ('  (payload not recorded by usbmon)' if e.truncated else ''))
            if e.data:
                print(hexdump(e.data, args.payload_bytes))
            j = idx + 1
            ins = collections.defaultdict(list)
            while j < len(stream) and stream[j].dir == 'i':
                ins[stream[j].ep].append(stream[j])
                j += 1
            for ep in sorted(ins):
                parts = ins[ep]
                total = sum(p.length for p in parts)
                print(f'    -> {role(ep, "i", "B")}: {len(parts)} transfer(s), '
                      f'{total} bytes, dt={(parts[0].ts - e.ts) / 1e3:.3f} ms')
                if parts[0].data:
                    print(hexdump(parts[0].data, args.payload_bytes, '       '))
            idx = j

    print()
    print('--- 4. OUT command histograms ---')
    firsts = collections.Counter()
    lens = collections.Counter()
    for e in stream:
        if e.dir == 'o':
            lens[e.length] += 1
            if e.data:
                firsts[e.data[:2].hex()] += 1
    print('  first two bytes:', ', '.join(f'{k}:{v}' for k, v in sorted(firsts.items())) or '(none)')
    print('  lengths        :', ', '.join(f'{l}B x{n}' for l, n in sorted(lens.items())) or '(none)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
