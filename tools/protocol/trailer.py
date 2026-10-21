import math
from usbmon import read, hx
names = ['b-simplex-lineart-200','c-duplex-lineart-200','e-simplex-lineart-300','f-simplex-gray-200']
base = "/home/tauboga/Projekte/Kodak i2600/captures/pcapng/%s.pcapng"
sizes = []
for n in names:
    us = read(base % n, dev=6)
    for ep in (0x82, 0x86):
        tr = [u for u in us if u.type=='C' and u.xfer==3 and u.ep==ep and u.status==0]
        if not tr: continue
        tot = 0; b = 0
        for u in tr:
            tot += u.length; b += 1
            if u.length < 16384:
                full = u.len_cap >= u.length
                tag = '%s ep%02x b%d' % (n[:18], ep, b)
                if full:
                    d = u.data
                    dup = d[-12:-8] == d[-8:-4]
                    print('%-28s size=%9d  dup(-12:-8)==(-8:-4): %-5s  last16: %s'
                          % (tag, tot, dup, hx(d[-16:])))
                sizes.append(tot)
                tot = 0; b = 0
sizes = sorted(set(sizes))
for T in (0, 4, 8):
    g = 0
    for s in sizes: g = math.gcd(g, s - T)
    print('trailer=%d bytes -> gcd of payload sizes = %d   (payload/gcd: %s)'
          % (T, g, [ (s-T)//g for s in sizes ] if g else '-'))
