import sys, collections
from usbmon import read, XFER
for path in sys.argv[1:]:
    us = read(path, dev=6)
    print('=== %s' % path.split('/')[-1])
    tot = collections.Counter(); cnt = collections.Counter(); trunc = 0
    sizes = collections.defaultdict(collections.Counter)
    for u in us:
        if u.type == 'C' and u.xfer == 3:
            k = '0x%02x' % u.ep
            tot[k] += u.length; cnt[k] += 1
            sizes[k][u.length] += 1
            if u.len_cap < u.length: trunc += 1
    for k in sorted(tot):
        print('  %s  %4d transfers  %12d bytes (urb_len)' % (k, cnt[k], tot[k]))
        for sz, n in sorted(sizes[k].items(), reverse=True)[:6]:
            print('        size %7d x %d' % (sz, n))
        if len(sizes[k]) > 6: print('        ... %d distinct sizes total' % len(sizes[k]))
    print('  truncated-in-capture URBs:', trunc)
