import sys
from usbmon import read, hx
path = sys.argv[1]
us = read(path, dev=6)
# completed bulk IN, in order
for ep in (0x82, 0x86):
    tr = [u for u in us if u.type=='C' and u.xfer==3 and u.ep==ep and u.status==0]
    if not tr: continue
    print('=== EP 0x%02x : %d completed transfers' % (ep, len(tr)))
    # burst = ends with a transfer shorter than 16384
    bn = 0; cnt = 0; tot = 0; t0 = tr[0].ts; first = None
    for u in tr:
        if cnt == 0:
            t0 = u.ts; first = u
        cnt += 1; tot += u.length
        if u.length < 16384:
            bn += 1
            print('  burst %d: %4d transfers %9d B  t=%7.3f..%7.3f  last=%d' %
                  (bn, cnt, tot, t0, u.ts, u.length))
            print('        first 48 B of burst: %s' % hx(first.data[:48]))
            print('        last  32 B captured: %s' % hx(u.data[-32:] if len(u.data)>=32 else u.data))
            cnt = 0; tot = 0
    if cnt:
        print('  burst %d (unterminated): %d transfers %d B t=%.3f..' % (bn+1, cnt, tot, t0))
