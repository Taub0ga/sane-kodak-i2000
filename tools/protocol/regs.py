import re, subprocess, sys, struct
from ctrl import transfers
from usbmon import hx

# ---- build address -> name map from devicemanager.so .rodata constants
path = 'driver/opt/kodak/kds_i2000/lib/devicemanager.so'
data = open(path,'rb').read()
e_shoff, = struct.unpack_from('<Q', data, 0x28)
e_shentsize, = struct.unpack_from('<H', data, 0x3a)
e_shnum, = struct.unpack_from('<H', data, 0x3c)
secs = [struct.unpack_from('<QQQ', data, e_shoff+i*e_shentsize+0x10) for i in range(e_shnum)]
def v2o(v):
    for a,o,s in secs:
        if a and a <= v < a+s: return o+(v-a)
REG = {}
for line in subprocess.run(['nm','-SC','--defined-only',path],
                           capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'^([0-9a-f]+) 0*2 ([rR]) (DeviceManager::\w*FpgaRegister::(\S+))$', line)
    if not m: continue
    val, = struct.unpack_from('<H', data, v2o(int(m.group(1),16)))
    REG[val] = m.group(4)

def name(a):
    if a in REG: return REG[a]
    # fall back to "nearest named register + offset"
    lo = [k for k in REG if k < a and (a - k) < 0x40]
    if lo:
        k = max(lo); return '%s+%d' % (REG[k], a-k)
    return '?'

cap = sys.argv[1]
print('# FPGA-Registerzugriffe (bRequest 0xa3), benannt aus devicemanager.so')
print('# wValue = Startadresse, wLength = Zahl der Register (je 1 Byte)\n')
for t in transfers(cap):
    if (t['bmr'] & 0x60) != 0x40 or t['breq'] != 0xa3: continue
    d = 'IN ' if t['bmr'] & 0x80 else 'OUT'
    a, n = t['wval'], t['wlen']
    if n <= 4:
        names = ', '.join(name(a+i) for i in range(max(n,1)))
    else:
        names = '%s .. %s  (%d Register)' % (name(a), name(a+n-1), n)
    print('%8.3f %s 0x%04x [%2d]  %-22s  %s' % (t['ts'], d, a, n,
          hx(t['data'][:12]) + ('...' if len(t['data'])>12 else ''), names))
