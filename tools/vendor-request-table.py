#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
"""Build a table of the vendor-specific control requests across captures.

Usage:  vendor-request-table.py capture.usbmon...

For every (direction, bRequest) pair it reports how often it occurs in each
capture, the wValue/wIndex values seen and the wLength values seen.  Vendor
requests are those with bmRequestType bits 6:5 == 01.
"""
import collections
import os
import re
import sys

LINE = re.compile(
    r'^(?P<tag>\S+)\s+(?P<ts>\d+)\s+(?P<ev>[SCE])\s+'
    r'(?P<type>[CZIB])(?P<dir>[io]):(?P<bus>\d+):(?P<dev>\d+):(?P<ep>\d+)\s*'
    r'(?P<rest>.*)$')


def setups(path):
    """Yield (dev, bmRequestType, bRequest, wValue, wIndex, wLength)."""
    for raw in open(path, errors='replace'):
        m = LINE.match(raw.rstrip('\n'))
        if not m:
            continue
        rest = m.group('rest').split()
        if not rest or rest[0] != 's':
            continue
        bm, br, wval, widx, wlen = rest[1:6]
        yield (int(m.group('dev')), int(bm, 16), int(br, 16),
               wval, widx, int(wlen, 16))


def main(paths):
    labels = [os.path.basename(p).split('.')[0] for p in paths]
    rows = collections.defaultdict(lambda: {
        'count': collections.Counter(),
        'wvalue': set(), 'windex': set(), 'wlength': set()})

    for path, label in zip(paths, labels):
        allsetups = list(setups(path))
        # the scanner is the device that issues vendor requests
        vendor_dev = collections.Counter(
            d for d, bm, *_ in allsetups if bm & 0x60 == 0x40)
        if not vendor_dev:
            continue
        dev = vendor_dev.most_common(1)[0][0]
        for d, bm, br, wval, widx, wlen in allsetups:
            if d != dev or bm & 0x60 != 0x40:
                continue
            key = ('IN' if bm & 0x80 else 'OUT', br)
            r = rows[key]
            r['count'][label] += 1
            r['wvalue'].add(wval)
            r['windex'].add(widx)
            r['wlength'].add(wlen)

    hdr = f'{"dir":<4} {"bReq":<6}'
    for label in labels:
        hdr += f' {label[:14]:>14}'
    hdr += '  wLength        wValue / wIndex'
    print(hdr)
    print('-' * len(hdr))
    for (dr, br) in sorted(rows, key=lambda k: (k[1], k[0])):
        r = rows[(dr, br)]
        line = f'{dr:<4} 0x{br:02x}  '
        for label in labels:
            line += f' {r["count"].get(label, 0):>14}'
        lens = ','.join(str(x) for x in sorted(r['wlength']))
        vals = sorted(r['wvalue'])
        idxs = sorted(r['windex'])
        vtxt = ','.join(vals[:6]) + ('...' if len(vals) > 6 else '')
        itxt = ','.join(idxs[:4]) + ('...' if len(idxs) > 4 else '')
        line += f'  {lens:<14} {vtxt} / {itxt}'
        print(line)
    print()
    print(f'{len(rows)} distinct vendor requests; '
          f'{sum(sum(r["count"].values()) for r in rows.values())} transfers')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1:])
