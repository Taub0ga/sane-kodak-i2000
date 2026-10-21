# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
import sys, statistics
from usbmon import read
path = sys.argv[1]; ep = int(sys.argv[2], 16)
us = read(path, dev=6)
tr = [u for u in us if u.type=='C' and u.xfer==3 and u.ep==ep and u.status==0]
blk = 0; pos = 0; prof = []
print('block profile: mean of captured bytes per transfer, 1 char per transfer')
print('  legend: . <20   : <60   - <110   + <170   # <220   @ >=220')
def ch(m):
    return '.' if m<20 else ':' if m<60 else '-' if m<110 else '+' if m<170 else '#' if m<220 else '@'
line = ''; tot = 0; k = 0
for u in tr:
    m = statistics.mean(u.data) if u.len_cap else 0
    line += ch(m); tot += u.length; k += 1
    if u.length < 16384:
        blk += 1
        print('  block %d (%d xfers, %d B):' % (blk, k, tot))
        for i in range(0, len(line), 100):
            print('    %4d %s' % (i, line[i:i+100]))
        line = ''; tot = 0; k = 0
