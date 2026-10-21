# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
"""Minimal reader for pcapng captures of Linux usbmon (LINKTYPE_USB_LINUX_MMAPPED=220)."""
import struct, sys

def _blocks(buf):
    off = 0
    endian = '<'
    while off + 12 <= len(buf):
        btype, = struct.unpack_from(endian + 'I', buf, off)
        if btype == 0x0A0D0D0A:  # SHB: determine endianness
            bom, = struct.unpack_from('<I', buf, off + 8)
            endian = '<' if bom == 0x1A2B3C4D else '>'
        blen, = struct.unpack_from(endian + 'I', buf, off + 4)
        if blen < 12 or off + blen > len(buf):
            break
        yield btype, buf[off + 8: off + blen - 4], endian
        off += blen

class Urb:
    __slots__ = ('n','id','type','xfer','ep','dev','bus','setup','ts','status',
                 'length','len_cap','data')
    def __repr__(self):
        return ('Urb(#%d %s xfer=%d ep=0x%02x dev=%d len=%d cap=%d st=%d)'
                % (self.n, self.type, self.xfer, self.ep, self.dev,
                   self.length, self.len_cap, self.status))

XFER = {0: 'ISO', 1: 'INTR', 2: 'CTRL', 3: 'BULK'}

def read(path, dev=None):
    with open(path, 'rb') as fh:
        buf = fh.read()
    n = 0
    t0 = None
    out = []
    for btype, body, endian in _blocks(buf):
        if btype != 6:  # Enhanced Packet Block
            continue
        caplen, = struct.unpack_from(endian + 'I', body, 12)
        pkt = body[20:20 + caplen]
        n += 1
        if len(pkt) < 64:
            continue
        u = Urb()
        u.n = n
        u.id, = struct.unpack_from('<Q', pkt, 0)
        u.type = chr(pkt[8])
        u.xfer = pkt[9]
        u.ep = pkt[10]
        u.dev = pkt[11]
        u.bus, = struct.unpack_from('<H', pkt, 12)
        flag_setup, flag_data = pkt[14], pkt[15]
        sec, = struct.unpack_from('<q', pkt, 16)
        usec, = struct.unpack_from('<i', pkt, 24)
        u.status, = struct.unpack_from('<i', pkt, 28)
        u.length, = struct.unpack_from('<I', pkt, 32)
        u.len_cap, = struct.unpack_from('<I', pkt, 36)
        u.setup = pkt[40:48] if flag_setup == 0 else None
        u.data = pkt[64:64 + u.len_cap]
        ts = sec + usec / 1e6
        if t0 is None:
            t0 = ts
        u.ts = ts - t0
        if dev is None or u.dev == dev:
            out.append(u)
    return out

def ctrl(u):
    """Decode setup packet -> (bmRequestType, bRequest, wValue, wIndex, wLength)."""
    if u.setup is None:
        return None
    return struct.unpack('<BBHHH', u.setup)

def hx(b, sep=' '):
    return sep.join('%02x' % c for c in b)

if __name__ == '__main__':
    for u in read(sys.argv[1], dev=6):
        print(u, ctrl(u), hx(u.data[:32]))
