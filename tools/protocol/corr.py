import statistics, itertools
from usbmon import read
base = "/home/tauboga/Projekte/Kodak i2600/captures/pcapng/%s.pcapng"
def blocks(name, ep):
    us = read(base % name, dev=6)
    tr = [u for u in us if u.type=='C' and u.xfer==3 and u.ep==ep and u.status==0]
    out = []; cur = []
    for u in tr:
        cur.append(statistics.mean(u.data) if u.len_cap else 0.0)
        if u.length < 16384:
            out.append(cur); cur = []
    return out
sets = {}
for name, ep in [('b-simplex-lineart-200',0x82), ('c-duplex-lineart-200',0x82),
                 ('c-duplex-lineart-200',0x86), ('f-simplex-gray-200',0x82),
                 ('e-simplex-lineart-300',0x82)]:
    for i, b in enumerate(blocks(name, ep), 1):
        if len(b) > 100:
            sets['%s/%02x/b%d' % (name.split('-')[0], ep, i)] = b
def r(a, b):
    n = min(len(a), len(b)); a = a[:n]; b = b[:n]
    ma, mb = statistics.mean(a), statistics.mean(b)
    num = sum((x-ma)*(y-mb) for x, y in zip(a, b))
    da = sum((x-ma)**2 for x in a) ** .5
    db = sum((y-mb)**2 for y in b) ** .5
    return num/(da*db) if da and db else 0
keys = sorted(sets)
print('pairwise correlation of per-transfer brightness profiles (r > 0.9 = same physical sheet)')
print('                 ' + ' '.join('%10s' % k.split('/')[0][:4]+k.split('/')[2] for k in keys))
for k1 in keys:
    print('%-16s %s' % (k1, ' '.join('%10.2f' % r(sets[k1], sets[k2]) for k2 in keys)))
