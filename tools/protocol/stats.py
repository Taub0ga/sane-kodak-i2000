# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
import sys, collections
from usbmon import read, ctrl, hx, XFER
for path in sys.argv[1:]:
    us = read(path, dev=6)
    print('=== %s : %d urbs (dev 6)' % (path.split('/')[-1], len(us)))
    c = collections.Counter()
    byx = collections.Counter(); bytes_ = collections.Counter()
    for u in us:
        c[(u.type, XFER.get(u.xfer), '0x%02x' % u.ep)] += 1
        if u.type == 'C' and u.status == 0 and u.ep & 0x80:
            bytes_['0x%02x' % u.ep] += u.len_cap
    for k, v in sorted(c.items()):
        print('   %s %-5s %s  %5d' % (k[0], k[1], k[2], v))
    print('   bytes IN completed:', dict(bytes_))
