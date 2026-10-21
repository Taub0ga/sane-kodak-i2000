from usbmon import read
from collections import Counter
us = read("/home/tauboga/Projekte/Kodak i2600/captures/pcapng/b-simplex-lineart-200.pcapng", dev=6)
tr = [u for u in us if u.type=='C' and u.xfer==3 and u.ep==0x82 and u.status==0]
# dark-reference burst = first 4 transfers; take transfer 0 (contiguous 4096 B from block start)
d = tr[0].data
print('dark-ref chunk, %d bytes captured' % len(d))
print('0xff byte positions:', [i for i,v in enumerate(d) if v == 0xff][:40])
print('positions of b"\\x01\\xff":', [i for i in range(len(d)-1) if d[i]==1 and d[i+1]==0xff][:40])
print('byte histogram top:', Counter(d).most_common(12))
print()
print('first 3 chunks of dark ref: bytes [0:16] and [500:520]')
for k in range(3):
    dd = tr[k].data
    print('  chunk%d [0:16]   %s' % (k, ' '.join('%02x'%c for c in dd[:16])))
    print('  chunk%d [496:528] %s' % (k, ' '.join('%02x'%c for c in dd[496:528])))
print()
# Does 508 show up as a period anywhere? check offsets of max values
print('max byte value in dark-ref chunk 0: 0x%02x' % max(d))
