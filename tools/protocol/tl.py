import sys, collections
from usbmon import read, ctrl, hx

def timeline(path, collapse=True):
    us = read(path, dev=6)
    pend = {}
    ev = []
    bulk = collections.defaultdict(lambda: [0,0,None])   # ep -> [xfers, bytes, t0]
    for u in us:
        if u.xfer == 2:
            if u.type == 'S':
                pend[u.id] = u; continue
            s = pend.pop(u.id, None)
            if s is None or s.setup is None: continue
            bmr, breq, wval, widx, wlen = ctrl(s)
            if (bmr & 0x60) != 0x40:
                ev.append((s.ts, 'STD ', 'bmR=%02x bReq=%d wVal=%04x' % (bmr, breq, wval), b''))
                continue
            data = s.data if not (bmr & 0x80) else u.data
            d = 'OUT' if not (bmr & 0x80) else 'IN '
            ev.append((s.ts, d, 'req=%02x val=%04x idx=%04x len=%d' % (breq, wval, widx, wlen), bytes(data)))
        elif u.xfer == 1 and u.type == 'C' and u.status == 0 and u.len_cap:
            ev.append((u.ts, 'INTR', 'ep 0x%02x' % u.ep, bytes(u.data)))
        elif u.xfer == 3 and u.type == 'C':
            b = bulk[u.ep]
            if b[2] is None: b[2] = u.ts
            b[0] += 1; b[1] += u.length
            if u.length < 16384:
                ev.append((b[2], 'BULK', 'ep 0x%02x  %d xfers, %d B, until t=%.3f%s'
                           % (u.ep, b[0], b[1], u.ts, '' if u.status==0 else ' (status %d)' % u.status), b''))
                bulk[u.ep] = [0,0,None]
    for ep, b in bulk.items():
        if b[0]: ev.append((b[2], 'BULK', 'ep 0x%02x %d xfers %d B (unterminated)' % (ep, b[0], b[1]), b''))
    ev.sort(key=lambda e: e[0])
    out = []
    prev = None; rep = 0
    for ts, kind, desc, data in ev:
        key = (kind, desc.split(' len=')[0], data[:4] if kind=='INTR' else b'')
        if collapse and prev == key:
            rep += 1; continue
        if rep:
            out.append('        ... previous repeated %d more time(s)' % rep); rep = 0
        prev = key
        line = '%8.3f  %-4s %-40s' % (ts, kind, desc)
        if data:
            line += ' | ' + hx(data[:48]) + ('...' if len(data) > 48 else '')
        out.append(line)
    if rep: out.append('        ... previous repeated %d more time(s)' % rep)
    return out

for l in timeline(sys.argv[1]):
    print(l)
