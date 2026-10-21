"""Decode the image footer of every fully captured image block of a pcapng.

Reimplements DeviceManager::Image::ParseImageFooter() (devicemanager.so 0x11af30).
The footer sits at the end of a logical image block and is read BACKWARD.
Each record is 4 bytes: <valueHi> <valueLo> <01> <tag>.

Usage: python3 footer.py captures/pcapng/b-simplex-lineart-200.pcapng [...]
"""
import sys
from usbmon import read

DOCCOUNTSIZE = 4          # <int name='documentcountsize'>4</int> in devicemanager.so

def be16(b, i): return (b[i] << 8) | b[i+1]
def be32(b, i): return (b[i] << 24) | (b[i+1] << 16) | (b[i+2] << 8) | b[i+3]

# ImageFooterTags::ResolutionToDPI (devicemanager.so 0x11a3d0)
DPI = {0: 600, 1: 400, 2: 300, 3: 200, 4: 150}

def parse(buf, total_len, verbose=True):
    """buf = the last (fully captured) transfer of the block, total_len = block size."""
    f = {}
    log = []
    off = total_len - len(buf)        # absolute offset of buf[0] inside the block
    i = len(buf) - 1
    while i > 0:
        t = buf[i]
        if t == 0x00:                                     # padding
            log.append((i, '0x00 pad')); i -= 2; continue
        if t == 0x01:                                     # start-of-footer marker
            f['footer_bytes'] = total_len - (off + i - 1)
            log.append((i, '0x01 FOOTER START -> %d footer bytes' % f['footer_bytes']))
            break
        if t == 0xff:                                     # junk words + skip
            mult = buf[i-1] * 2
            v = be16(buf, i-3); f['JunkWords'] = v
            log.append((i, '0xff JunkWords=%d x%d -> %d junk bytes' % (v, mult, v*mult)))
            i = i - 4 - v * mult; continue
        if t == 0xfe:
            f['Format'] = be16(buf, i-3); log.append((i, '0xfe Format=%d' % f['Format'])); i -= 4; continue
        if t == 0xfd:
            f['Type'] = be16(buf, i-3); log.append((i, '0xfd Type=%d' % f['Type'])); i -= 4; continue
        if t == 0xfc:                                     # width, in pixel pairs
            w = be16(buf, i-3); f['WidthPairs'] = w; f['WidthPixels'] = w * 2
            log.append((i, '0xfc Width=%d pixel pairs -> %d pixels' % (w, w*2))); i -= 4; continue
        if t == 0xfa:
            r = be16(buf, i-3); f['XResCode'] = r; f['XResDpi'] = DPI.get(r)
            log.append((i, '0xfa XRes code=%d -> %s dpi' % (r, DPI.get(r)))); i -= 4; continue
        if t == 0xf9:
            r = be16(buf, i-3); f['YResCode'] = r; f['YResDpi'] = DPI.get(r)
            log.append((i, '0xf9 YRes code=%d -> %s dpi' % (r, DPI.get(r)))); i -= 4; continue
        if t == 0xf8:                                     # print string + printer index
            n = buf[i-1]
            log.append((i, '0xf8 PrintString, %d chars' % n)); i = i - 2 - 2*n; continue
        if t == 0xf7:
            log.append((i, '0xf7 (value ignored by the driver)')); i -= 4; continue
        if t == 0xf6:
            v = be16(buf, i-3); f['Segment'] = v & 0x7fff; f['LastSegment'] = bool(v & 0x8000)
            log.append((i, '0xf6 Segment=%d LastSegment=%s' % (f['Segment'], f['LastSegment'])))
            i -= 4; continue
        if t == 0xf5:
            if DOCCOUNTSIZE == 4:
                f['DocumentCount'] = be32(buf, i-5); i -= 6
            else:
                f['DocumentCount'] = be16(buf, i-3); i -= 4
            log.append((i, '0xf5 DocumentCount=%d' % f['DocumentCount'])); continue
        if t == 0xf4:
            f['f4_a'] = be16(buf, i-3); f['f4_b'] = be16(buf, i-5)
            log.append((i, '0xf4 (unnamed) %d / %d' % (f['f4_a'], f['f4_b']))); i -= 6; continue
        log.append((i, 'UNKNOWN tag 0x%02x -> abort' % t)); break
    if verbose:
        for a, s in log:
            print('      @%-6d %s' % (a, s))
    return f

def blocks(cap):
    """Yield (endpoint, index, total bytes, last URB) per logical image block."""
    cur = {}
    for u in read(cap, dev=6):
        if u.type != 'C' or u.xfer != 3 or u.ep not in (0x82, 0x86) or u.status != 0:
            continue
        cur.setdefault(u.ep, []).append(u)
    for ep in sorted(cur):
        blk = []; n = 0
        for u in cur[ep]:
            blk.append(u)
            if u.length < 16384:                      # short packet = end of block
                n += 1
                yield ep, n, sum(x.length for x in blk), u
                blk = []

if __name__ == '__main__':
    for cap in sys.argv[1:]:
        print('=== %s' % cap.split('/')[-1])
        for ep, n, tot, u in blocks(cap):
            if u.len_cap < u.length:
                print('  ep 0x%02x block %d: %d bytes, last transfer %d TRUNCATED in capture'
                      % (ep, n, tot, u.length))
                continue
            print('  ep 0x%02x block %d: %d bytes, last transfer %d' % (ep, n, tot, u.length))
            f = parse(bytes(u.data), tot)
            img = tot - f.get('footer_bytes', 0)
            print('      fields: %s' % f)
            print('      image data = %d - %d = %d bytes' % (tot, f.get('footer_bytes', 0), img))
            w = f.get('WidthPixels')
            if w:
                print('      %d / %d bytes per line = %s lines%s'
                      % (img, w, img // w, '' if img % w == 0 else '  (remainder %d!)' % (img % w)))
            print('      block length mod 508 = %d' % (tot % 508))
