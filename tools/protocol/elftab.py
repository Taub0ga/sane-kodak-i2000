# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
import struct, sys, subprocess

path = sys.argv[1]
data = open(path,'rb').read()

# parse ELF64 section headers to map vaddr->file offset
e_shoff, = struct.unpack_from('<Q', data, 0x28)
e_shentsize, = struct.unpack_from('<H', data, 0x3a)
e_shnum, = struct.unpack_from('<H', data, 0x3c)
secs = []
for i in range(e_shnum):
    o = e_shoff + i*e_shentsize
    sh_addr, sh_off, sh_size = struct.unpack_from('<QQQ', data, o+0x10)
    secs.append((sh_addr, sh_off, sh_size))
def v2o(v):
    for a, o, s in secs:
        if a and a <= v < a + s:
            return o + (v - a)
    return None

# relocations (R_X86_64_RELATIVE store the target in r_addend)
rel = {}
out = subprocess.run(['readelf','-r','-W',path], capture_output=True, text=True).stdout
for line in out.splitlines():
    f = line.split()
    if len(f) >= 4 and f[1].endswith('00000008'):   # R_X86_64_RELATIVE
        try: rel[int(f[0],16)] = int(f[3],16)
        except ValueError: pass

def cstr(v):
    o = v2o(v)
    if o is None: return None
    end = data.index(b'\0', o)
    return data[o:end].decode('latin1')

def table(addr, size, stride, idfmt='B'):
    o = v2o(addr)
    n = size // stride
    rows = []
    for i in range(n):
        rec = data[o + i*stride : o + (i+1)*stride]
        idv = rec[0] if idfmt=='B' else struct.unpack_from('<I', rec, 0)[0]
        ptr_at = addr + i*stride + 8
        p = rel.get(ptr_at) or struct.unpack_from('<Q', rec, 8)[0]
        rows.append((idv, struct.unpack_from('<I', rec, 0)[0], cstr(p), rec[:8].hex()))
    return rows

for name, addr, size, stride in [('s_eventtidtable', 0x54ed80, 0x200, 16),
                                 ('s_commandidtable', 0x54ef80, 0x1d0, 16)]:
    print('=== %s  (%d entries of %d bytes)' % (name, size//stride, stride))
    for idv, w32, s, raw in table(addr, size, stride):
        print('   0x%02x (u32=0x%08x) raw=%s  %s' % (idv, w32, raw, s))
