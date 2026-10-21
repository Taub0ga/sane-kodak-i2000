#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
"""Dump the payload of every vendor control request in a pcapng capture.

Usage:  vendor-payloads.py capture.pcapng [--device N] [--max-bytes N]
                                          [--unique] [--request 0xNN]

Needs tshark.  For each vendor request (bmRequestType bits 6:5 == 01) it
prints direction, bRequest, wValue, wIndex, wLength and the data.  With
--unique, repeated identical (request, wValue, wIndex, payload) tuples are
collapsed and counted, which turns a scan into a short inventory.
"""
import argparse
import collections
import shutil
import subprocess
import sys

FIELDS = ['frame.number', 'usb.device_address', 'usb.bmRequestType',
          'usb.setup.bRequest', 'usb.setup.wValue', 'usb.setup.wIndex',
          'usb.setup.wLength', 'usb.data_fragment', 'usb.control.Response',
          'usb.request_in']


def run(path):
    tshark = shutil.which('tshark')
    if not tshark:
        sys.exit('tshark not found (install wireshark-cli)')
    cmd = [tshark, '-r', path, '-T', 'fields', '-E', 'separator=\t']
    for f in FIELDS:
        cmd += ['-e', f]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode:
        sys.exit(out.stderr.strip())
    return out.stdout.splitlines()


def hexdump(data, limit, indent='      '):
    lines = []
    for off in range(0, min(len(data), limit), 16):
        c = data[off:off + 16]
        lines.append(f'{indent}{off:04x}  '
                     + ' '.join(f'{b:02x}' for b in c).ljust(47) + '  |'
                     + ''.join(chr(b) if 32 <= b < 127 else '.' for b in c)
                     + '|')
    if len(data) > limit:
        lines.append(f'{indent}... {len(data) - limit} more bytes')
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--device', type=int)
    ap.add_argument('--max-bytes', type=int, default=256)
    ap.add_argument('--unique', action='store_true')
    ap.add_argument('--request', type=lambda s: int(s, 0))
    args = ap.parse_args()

    # A device-to-host request carries its data on the completion frame, which
    # has no setup fields of its own; Wireshark links the two with
    # usb.request_in.  So collect the requests first, then attach the replies.
    rows = []
    byframe = {}
    replies = {}
    for line in run(args.capture):
        col = line.split('\t')
        col += [''] * (len(FIELDS) - len(col))
        (fnum, dev, bm, br, wval, widx, wlen, frag, resp, rin) = col[:10]
        if not fnum.isdigit():
            continue
        fnum = int(fnum)
        if bm and br:
            try:
                bmv = int(bm, 0)
            except ValueError:
                continue
            if bmv & 0x60 != 0x40:
                continue
            row = [fnum, int(dev) if dev.isdigit() else -1, bmv, int(br, 0),
                   wval or '0x0000', widx or '0',
                   int(wlen, 0) if wlen else 0, (frag or '').replace(':', '')]
            rows.append(row)
            byframe[fnum] = row
        elif resp and rin.isdigit():
            replies[int(rin)] = resp.replace(':', '')
    for req_frame, data in replies.items():
        row = byframe.get(req_frame)
        if row is not None and not row[7]:
            row[7] = data
    rows = [tuple(r) for r in rows]

    if not rows:
        print('no vendor control requests found')
        return
    dev = args.device
    if dev is None:
        dev = collections.Counter(r[1] for r in rows).most_common(1)[0][0]
    rows = [r for r in rows if r[1] == dev]
    if args.request is not None:
        rows = [r for r in rows if r[3] == args.request]

    print(f'{args.capture}: {len(rows)} vendor control requests, device {dev}')

    if args.unique:
        seen = collections.Counter()
        order = []
        for _f, _d, bm, br, wval, widx, wlen, data in rows:
            key = (bm & 0x80, br, wval, widx, wlen, data)
            if key not in seen:
                order.append(key)
            seen[key] += 1
        for key in order:
            inbit, br, wval, widx, wlen, data = key
            arrow = 'IN ' if inbit else 'OUT'
            idx = int(widx, 0) if widx else 0
            print(f'\n  x{seen[key]:<4} {arrow} bRequest=0x{br:02x} '
                  f'wValue={wval} wIndex=0x{idx:04x} wLength={wlen}')
            if data:
                print(hexdump(bytes.fromhex(data), args.max_bytes))
    else:
        for fnum, _d, bm, br, wval, widx, wlen, data in rows:
            arrow = 'IN ' if bm & 0x80 else 'OUT'
            idx = int(widx, 0) if widx else 0
            print(f'\n  [{fnum}] {arrow} bRequest=0x{br:02x} wValue={wval} '
                  f'wIndex=0x{idx:04x} wLength={wlen}')
            if data:
                print(hexdump(bytes.fromhex(data), args.max_bytes))


if __name__ == '__main__':
    main()
