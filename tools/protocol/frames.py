# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
import sys
from usbmon import read, hx
us = read(sys.argv[1], dev=6)
ep = int(sys.argv[2], 16)
tr = [u for u in us if u.type=='C' and u.xfer==3 and u.ep==ep and u.status==0]
print('first 16 B of the first 8 transfers of each burst, and of a few mid-burst transfers')
bidx = 0; cnt = 0
for i, u in enumerate(tr):
    if cnt < 6 or u.length < 16384:
        print('  b%d t%-4d len=%6d cap=%5d | %s' % (bidx, cnt, u.length, u.len_cap, hx(u.data[:16])))
    elif cnt in (100, 300, 600):
        print('  b%d t%-4d len=%6d cap=%5d | %s   (mid)' % (bidx, cnt, u.length, u.len_cap, hx(u.data[:16])))
    cnt += 1
    if u.length < 16384:
        print('      TAIL last 64 B: %s' % hx(u.data[-64:]))
        bidx += 1; cnt = 0
