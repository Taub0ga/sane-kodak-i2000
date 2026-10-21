import sys, collections
from ctrl import transfers
names = ['a-open-close','b-simplex-lineart-200','c-duplex-lineart-200',
         'd1-no-paper','e-simplex-lineart-300','f-simplex-gray-200']
base = "/home/tauboga/Projekte/Kodak i2600/captures/pcapng/%s.pcapng"
tab = collections.defaultdict(lambda: collections.Counter())
lens = collections.defaultdict(set)
vals = collections.defaultdict(set)
idxs = collections.defaultdict(set)
for n in names:
    for t in transfers(base % n):
        if (t['bmr'] & 0x60) != 0x40: continue
        key = (t['breq'], 'IN' if t['bmr'] & 0x80 else 'OUT')
        tab[key][n] += 1
        lens[key].add(t['wlen'])
        vals[key].add(t['wval']); idxs[key].add(t['widx'])
print('req dir | ' + ' '.join('%5s' % n[0] for n in names) + ' | wLength | wValue (distinct) | wIndex')
for key in sorted(tab):
    row = tab[key]
    nv = sorted(vals[key]); ni = sorted(idxs[key])
    def fmt(s):
        if len(s) <= 4: return ','.join('%04x' % x for x in s)
        return '%d distinct %04x..%04x' % (len(s), s[0], s[-1])
    print('0x%02x %-3s | %s | %s | %s | %s' % (
        key[0], key[1],
        ' '.join('%5d' % row.get(n, 0) for n in names),
        ','.join(str(x) for x in sorted(lens[key])),
        fmt(nv), fmt(ni)))
