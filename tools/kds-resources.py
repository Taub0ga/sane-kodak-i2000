#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
"""Parse the '@!@kds@!@' resource directory appended to Kodak kds_i2000 modules."""
import sys, os, struct

MAGIC = b'@!@kds@!@'
ENT   = 56          # name[32] + u64 offset + u64 size + 8 pad

def entries(data):
    end = data.rfind(MAGIC)
    if end < 0:
        return []
    out = []
    pos = end - ENT
    while pos >= 0:
        name, off, size = struct.unpack_from('<32sQQ', data, pos)
        name = name.split(b'\0', 1)[0]
        if not name or not all(32 <= c < 127 for c in name):
            break
        if off == 0 or off + size > len(data):
            break
        out.append((name.decode(), off, size))
        pos -= ENT
    out.reverse()
    return out

def main():
    path = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else None
    data = open(path, 'rb').read()
    es = entries(data)
    print(f"{path}: {len(es)} resources, dir ends at 0x{data.rfind(MAGIC):x}")
    print(f"{'name':<20} {'offset':>10} {'size':>10}  first 16 bytes")
    for name, off, size in es:
        print(f"{name:<20} 0x{off:08x} {size:>10}  {data[off:off+16].hex()}")
        if outdir:
            os.makedirs(outdir, exist_ok=True)
            open(os.path.join(outdir, name + '.bin'), 'wb').write(data[off:off+size])

main()
