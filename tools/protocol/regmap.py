# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
import struct, subprocess, sys, re
path = 'driver/opt/kodak/kds_i2000/lib/devicemanager.so'
data = open(path,'rb').read()
e_shoff, = struct.unpack_from('<Q', data, 0x28)
e_shentsize, = struct.unpack_from('<H', data, 0x3a)
e_shnum, = struct.unpack_from('<H', data, 0x3c)
secs = []
for i in range(e_shnum):
    o = e_shoff + i*e_shentsize
    a, off, size = struct.unpack_from('<QQQ', data, o+0x10)
    secs.append((a, off, size))
def v2o(v):
    for a, o, s in secs:
        if a and a <= v < a+s: return o + (v-a)
out = subprocess.run(['nm','-SC','--defined-only',path], capture_output=True, text=True).stdout
rows = []
for line in out.splitlines():
    m = re.match(r'^([0-9a-f]+) ([0-9a-f]+) ([rRdDbB]) (DeviceManager::\w*FpgaRegister::\S+)$', line)
    if not m: continue
    addr, size, kind, name = int(m.group(1),16), int(m.group(2),16), m.group(3), m.group(4)
    if size != 2 or kind not in 'rR': continue
    o = v2o(addr)
    val, = struct.unpack_from('<H', data, o)
    rows.append((val, name))
rows.sort()
print('%d FPGA register constants (name -> register address, little endian u16)\n' % len(rows))
for val, name in rows:
    print('  0x%04x  %s' % (val, name.replace('DeviceManager::','')))
