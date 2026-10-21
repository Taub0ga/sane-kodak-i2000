import sys, collections
from usbmon import read, ctrl, hx

def transfers(path, dev=None):
    """Pair control submit/complete into logical transfers.

    The device address differs per capture -- it is whatever the kernel
    assigned when the scanner was last plugged in -- so detect the busiest
    one instead of hardcoding it.  Pass dev= to override.
    """
    if dev is None:
        import collections
        probe = read(path)
        counts = collections.Counter(u.dev for u in probe if u.xfer in (2, 3))
        if not counts:
            return []
        dev = counts.most_common(1)[0][0]
    us = read(path, dev=dev)
    pend = {}
    out = []
    for u in us:
        if u.xfer != 2:
            continue
        if u.type == 'S':
            pend[u.id] = u
            continue
        s = pend.pop(u.id, None)
        if s is None or s.setup is None:
            continue
        bmr, breq, wval, widx, wlen = ctrl(s)
        data = s.data if not (bmr & 0x80) else u.data
        out.append(dict(frame=u.n, ts=s.ts, bmr=bmr, breq=breq, wval=wval,
                        widx=widx, wlen=wlen, status=u.status, data=bytes(data),
                        trunc=(u.len_cap < u.length)))
    return out

def label(t):
    return 'bmR=%02x req=%02x val=%04x idx=%04x len=%d' % (
        t['bmr'], t['breq'], t['wval'], t['widx'], t['wlen'])

if __name__ == '__main__':
    for path in sys.argv[1:]:
        ts = transfers(path)
        vend = [t for t in ts if (t['bmr'] & 0x60) == 0x40]
        std  = [t for t in ts if (t['bmr'] & 0x60) == 0x00]
        print('=== %s: %d control transfers (%d vendor, %d standard)'
              % (path.split('/')[-1], len(ts), len(vend), len(std)))
        bad = [t for t in ts if t['status'] != 0]
        print('   non-zero status:', collections.Counter(t['status'] for t in bad))
        print('   truncated:', sum(1 for t in ts if t['trunc']))
        print('   short data (len(data) != wLength):',
              sum(1 for t in ts if len(t['data']) != t['wlen']))
