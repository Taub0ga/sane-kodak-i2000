import re
lines = open('dm.dis').read().splitlines()
fn = None; funcs = []
for i, l in enumerate(lines):
    m = re.match(r'^([0-9a-f]+) <(.+)>:$', l)
    if m: funcs.append((i, m.group(2)))
def encl(i):
    lo, hi = 0, len(funcs)-1; best = None
    for j, n in funcs:
        if j < i: best = n
        else: break
    return best
MOV = re.compile(r'mov\s+\$(0x[0-9a-f]+|\d+),%(esi|edx|ecx|r8d|rsi|rdx|rcx)\b')
rows = []
for i, l in enumerate(lines):
    if 'call' in l and ('<DeviceManager::DeviceSettings::Get(unsigned char' in l
                        or '<DeviceManager::DeviceSettings::Set(unsigned char' in l):
        kind = 'Get(C0/IN )' if 'Get(' in l.split('<')[-1] else 'Set(40/OUT)'
        regs = {}
        for k in range(i-1, max(0, i-26), -1):
            if 'call' in lines[k] and k != i: break
            m = MOV.search(lines[k])
            if m:
                r = m.group(2).replace('rsi','esi').replace('rdx','edx').replace('rcx','ecx')
                regs.setdefault(r, int(m.group(1), 0))
        rows.append((kind, regs.get('esi'), regs.get('edx'), regs.get('ecx'),
                     regs.get('r8d'), encl(i)))
def h(v, w=2):
    return ('0x%0*x' % (w, v)) if v is not None else '  ?  '
rows.sort(key=lambda r: (r[1] if r[1] is not None else 999, r[0]))
print('%-11s %-6s %-7s %-7s %-7s %s' % ('direction','bReq','wValue','wIndex','cbData','calling function'))
for kind, req, val, idx, cb, f in rows:
    print('%-11s %-6s %-7s %-7s %-7s %s' % (kind, h(req), h(val,4), h(idx,4), h(cb,4), f))
