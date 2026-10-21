# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
"""Annotated static trace of a function from devicemanager.so.

For every call instruction inside a function it prints the callee, the string
literals and data symbols referenced on the way there, and the immediate
constants loaded into argument registers.  COsLog::Message calls carry the
source line number in %edx, so the second listing ("source-line order")
reconstructs the real order of the code - the address order of an optimised
binary is not the source order.

Usage:
    objdump -d -C --no-show-raw-insn devicemanager.so > dm.dis
    python3 dmtrace.py dm.dis devicemanager.so '^DeviceManager::Scanner::Setup' --src
"""
import re, sys, subprocess, struct, bisect
dis_path, so_path, pat = sys.argv[1], sys.argv[2], sys.argv[3]
bysrc = '--src' in sys.argv

data = open(so_path,'rb').read()
e_shoff, = struct.unpack_from('<Q', data, 0x28)
e_shentsize, = struct.unpack_from('<H', data, 0x3a)
e_shnum, = struct.unpack_from('<H', data, 0x3c)
secs=[]
for i in range(e_shnum):
    o=e_shoff+i*e_shentsize
    st,=struct.unpack_from('<I',data,o+4)
    a,off,size=struct.unpack_from('<QQQ',data,o+0x10)
    secs.append((a,off,size,st))
def v2o(v):
    for a,off,size,st in secs:
        if a and st!=8 and a<=v<a+size: return off+(v-a)
def cstr(v,maxlen=200):
    o=v2o(v)
    if o is None: return None
    e=data.find(b'\0',o,o+maxlen)
    if e<0: return None
    b=data[o:e]
    if not b: return ''
    if all(32<=c<127 or c in (9,) for c in b):
        return b.decode('ascii')
    return None
rel={}
for l in subprocess.run(['readelf','-rW',so_path],capture_output=True,text=True).stdout.splitlines():
    f=l.split()
    if len(f)>=4 and f[2]=='R_X86_64_RELATIVE': rel[int(f[0],16)]=int(f[3],16)
syms=[]
for l in subprocess.run(['nm','-SC','--defined-only',so_path],capture_output=True,text=True).stdout.splitlines():
    m=re.match(r'^([0-9a-f]+) ([0-9a-f]+) (\S) (.+)$',l)
    if m: syms.append((int(m.group(1),16),int(m.group(2),16),m.group(3),m.group(4)))
syms.sort(); addrs=[s[0] for s in syms]
def dsym(a):
    i=bisect.bisect_right(addrs,a)-1
    if i<0: return None
    b,s,k,n=syms[i]
    if s and a>=b+s: return None
    if k in 'tTwW': return None
    return n+(('+%d'%(a-b)) if a!=b else '')

lines=open(dis_path).read().splitlines()
blocks={}; order=[]
for i,l in enumerate(lines):
    m=re.match(r'^([0-9a-f]+) <(.+)>:$',l)
    if m: blocks[m.group(2)]=[i,None]; order.append(m.group(2))
for j,n in enumerate(order):
    blocks[n][1]=blocks[order[j+1]][0] if j+1<len(order) else len(lines)

IMMD=re.compile(r'^\$(0x[0-9a-f]+|\d+),%edx$')
IMM=re.compile(r'\$(0x[0-9a-f]+|\d+),%(e[a-d]x|esi|edi|r8d|r9d|r1[0-5]d)\b')
for t in [n for n in order if re.search(pat,n)]:
    s,e=blocks[t]
    print('='*110); print('=== %s' % t); print('='*110)
    items=[]
    pend=[]; lastdx=None
    for i in range(s+1,e):
        l=lines[i]
        m=re.match(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$',l)
        if not m: continue
        a,mn,rest=int(m.group(1),16),m.group(2),m.group(3)
        md=IMMD.search(rest)
        if md: lastdx=int(md.group(1),0)
        for mm in IMM.finditer(rest):
            pend.append('%s->%s'%(mm.group(1),mm.group(2)))
        mc=re.search(r'#\s+([0-9a-f]+)',rest)
        if mc and '(%rip)' in rest:
            tv=int(mc.group(1),16)
            ds=dsym(tv)
            if ds and ds.startswith('.got'):
                r=rel.get(tv)
                if r is not None:
                    d2=dsym(r)
                    if d2: pend.append('GOT->%s'%d2)
            elif ds: pend.append('DATA %s'%ds)
            else:
                sv=cstr(tv)
                if sv and len(sv)>1: pend.append('"%s"'%sv)
        if mn.startswith('call'):
            cm=re.search(r'<([^>]+)>',rest)
            tgt=cm.group(1) if cm else rest.strip()
            ln=lastdx if 'COsLog::Message' in tgt else None
            items.append((a,ln,tgt,pend[-12:]))
            pend=[]
    for a,ln,tgt,p in items:
        if 'COsLog::GetDebugLevel' in tgt: continue
        tag='L%-5s'%ln if ln else '      '
        print('  %6x %s %-64s %s' % (a,tag,tgt,' | '.join(p)))
    if bysrc:
        print('  ---- log messages in source-line order ----')
        logs=[(ln,a,p) for a,ln,tgt,p in items if ln and 'COsLog::Message' in tgt]
        for ln,a,p in sorted(logs):
            print('  L%-6d %6x  %s' % (ln,a,' | '.join(x for x in p if x.startswith('"'))))
    print()
