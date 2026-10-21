"""Compare the six 32-byte camera pages of a capture against the static tables
s_dmcamera{color,gs}{300,600}{master,slave}[DT] in devicemanager.so.

SetupScanner::SetCameraResolution() writes pages 0..5 of each camera from one
of these 192-byte (6 x 32) tables.

Usage: python3 camtab.py captures/pcapng/b-simplex-lineart-200.pcapng \
         [driver/opt/kodak/kds_i2000/lib/devicemanager.so]
"""
import sys, re, struct, subprocess
from ctrl import transfers

SO = sys.argv[2] if len(sys.argv) > 2 else 'driver/opt/kodak/kds_i2000/lib/devicemanager.so'

data = open(SO, 'rb').read()
e_shoff, = struct.unpack_from('<Q', data, 0x28)
e_shentsize, = struct.unpack_from('<H', data, 0x3a)
e_shnum, = struct.unpack_from('<H', data, 0x3c)
secs = []
for i in range(e_shnum):
    o = e_shoff + i * e_shentsize
    stype, = struct.unpack_from('<I', data, o + 4)
    a, off, size = struct.unpack_from('<QQQ', data, o + 0x10)
    secs.append((a, off, size, stype))
def v2o(v):
    for a, off, size, stype in secs:
        if a and stype != 8 and a <= v < a + size:
            return off + (v - a)

TABLES = {}
for line in subprocess.run(['nm', '-SC', '--defined-only', SO],
                           capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'^([0-9a-f]+) ([0-9a-f]+) \S (s_dmcamera\S+)$', line)
    if m:
        a, n = int(m.group(1), 16), int(m.group(2), 16)
        TABLES[m.group(3)] = data[v2o(a):v2o(a) + n]

def pages(cap):
    """{camera: {page: 32 bytes}} from the 32-byte 0xe0 writes of the capture."""
    out, sel = {}, {}
    for t in transfers(cap):
        if (t['bmr'] & 0x60) != 0x40 or t['breq'] != 0xe0:
            continue
        if t['wval'] == 0x1f and not (t['bmr'] & 0x80):
            sel[t['widx']] = t['data'][0]
        elif t['wval'] == 0x00 and t['wlen'] == 32 and not (t['bmr'] & 0x80):
            out.setdefault(t['widx'], {})[sel.get(t['widx'])] = bytes(t['data'])
    return out

if __name__ == '__main__':
    got = pages(sys.argv[1])
    for cam in sorted(got):
        pg = got[cam]
        if sorted(pg) != list(range(6)):
            print('camera %d: pages %s - skipping' % (cam, sorted(pg))); continue
        blob = b''.join(pg[i] for i in range(6))
        print('=== camera %d (%s), %d bytes written in pages 0..5'
              % (cam, 'front/master' if cam == 0 else 'rear/slave', len(blob)))
        scored = sorted((sum(1 for x, y in zip(blob, t) if x != y), n, t)
                        for n, t in TABLES.items())
        for diff, n, t in scored:
            print('    %-30s differing bytes: %3d / %d' % (n, diff, len(t)))
        diff, n, t = scored[0]
        print('    --> closest: %s' % n)
        for i in range(len(blob)):
            if blob[i] != t[i]:
                print('        page %d reg 0x%02x: table 0x%02x -> capture 0x%02x'
                      % (i // 32, i % 32, t[i], blob[i]))
