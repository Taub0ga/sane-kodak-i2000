# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
import math, collections
from usbmon import read, hx
names = ['b-simplex-lineart-200','c-duplex-lineart-200','e-simplex-lineart-300','f-simplex-gray-200']
base = "/home/tauboga/Projekte/Kodak i2600/captures/pcapng/%s.pcapng"
allb = []
for n in names:
    us = read(base % n, dev=6)
    for ep in (0x82, 0x86):
        tr = [u for u in us if u.type=='C' and u.xfer==3 and u.ep==ep and u.status==0]
        tot = 0; k = 0
        for u in tr:
            tot += u.length; k += 1
            if u.length < 16384:
                allb.append((n, '0x%02x'%ep, k, tot, u.data[-8:]))
                tot = 0; k = 0
        if tot: allb.append((n,'0x%02x'%ep, k, tot, b'(unterminated)'))
print('%-24s %-5s %5s %10s  last 8 bytes' % ('capture','ep','xfers','bytes'))
for n, ep, k, tot, tail in allb:
    print('%-24s %-5s %5d %10d  %s' % (n, ep, k, tot, hx(tail) if isinstance(tail,(bytes,bytearray)) else tail))
sizes = sorted({t for _,_,_,t,_ in allb})
print('\ndistinct burst sizes:', sizes)
big = [s for s in sizes if s > 100000]
g = 0
for i in range(1, len(big)):
    g = math.gcd(g, big[i]-big[0])
print('gcd of pairwise differences (image bursts only):', g)
g2 = 0
for s in sizes:
    g2 = math.gcd(g2, s)
print('gcd of all burst sizes incl. dark ref:', g2)
print('51824 factors:', [d for d in range(1,400) if 51824 % d == 0])
