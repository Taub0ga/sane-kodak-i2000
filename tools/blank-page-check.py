#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
"""Emulate i2600_autocrop + page_ink_percent on a PGM: what would i2600-scand
decide about this page?

Usage:  blank-page-check.py scan.pgm [guard]

`guard` is the dark-page bound from page_ink_percent() in i2600-scand.c, 150
since the shading calibration is loaded -- NATIVES-BACKEND.md section 17.
This is a reimplementation of C code in i2600-core.c and i2600-scand.c and
will drift if either changes; it is a measuring aid, not a second
implementation to rely on.
"""
import sys


def read_pnm(path):
    d = open(path, 'rb').read()
    tok, i = [], 0
    while len(tok) < 4:
        while i < len(d) and d[i:i+1].isspace():
            i += 1
        if d[i:i+1] == b'#':
            while d[i:i+1] != b'\n':
                i += 1
            continue
        j = i
        while j < len(d) and not d[j:j+1].isspace():
            j += 1
        tok.append(d[i:j])
        i = j
    i += 1
    magic, w, h = tok[0], int(tok[1]), int(tok[2])
    bpp = 3 if magic == b'P6' else 1
    return w, h, bpp, d[i:i + w*h*bpp]


def autocrop(px, w, lines, bpp=1):
    stride = w*bpp
    rowmean = []
    cnt = (stride + bpp*8 - 1)//(bpp*8)
    for y in range(lines):
        row = px[y*stride:(y+1)*stride]
        s = sum(row[i] for i in range(0, stride, bpp*8))
        rowmean.append(s//cnt)
    page = sorted(rowmean)[lines*9//10]
    thr = page//4
    if page < 40: return None
    ys = [y for y in range(lines) if rowmean[y] > thr]
    if not ys: return None
    y0, y1 = ys[0], ys[-1]
    x0 = x1 = -1
    for x in range(w):
        vals = [px[y*stride + x*bpp] for y in range(y0, y1+1, 16)]
        if vals and sum(vals)//len(vals) > thr:
            if x0 < 0: x0 = x
            x1 = x
    if x0 < 0: return None
    clean = page*9//10
    def lm(axis, idx, a, b):
        if axis == 1:
            vals = [px[idx*stride + x*bpp] for x in range(a, b+1)]
        else:
            vals = [px[y*stride + idx*bpp] for y in range(a, b+1)]
        return sum(vals)//len(vals) if vals else 0
    max_x, max_y = w//100+1, lines//100+1
    for _ in range(max_y):
        if y0 >= y1 or lm(1, y0, x0, x1) >= clean: break
        y0 += 1
    for _ in range(max_y):
        if y1 <= y0 or lm(1, y1, x0, x1) >= clean: break
        y1 -= 1
    for _ in range(max_x):
        if x0 >= x1 or lm(0, x0, y0, y1) >= clean: break
        x0 += 1
    for _ in range(max_x):
        if x1 <= x0 or lm(0, x1, y0, y1) >= clean: break
        x1 -= 1
    return x0, y0, x1, y1, page

def ink(px, w, lines, bpp=1, guard=200):
    mx, my = w//100, lines//100
    if w <= 2*mx or lines <= 2*my: return 100.0, 0, 0
    total = (w-2*mx)*(lines-2*my)
    hist = [0]*256; s = 0
    for y in range(my, lines-my):
        row = px[y*w*bpp:(y+1)*w*bpp]
        for x in range(mx, w-mx):
            v = row[x*bpp]; hist[v] += 1; s += v
    mean = s//total
    if mean < guard: return 100.0, mean, 0
    best = max(range(128, 256), key=lambda i: hist[i])
    if hist[best] == 0: return 100.0, mean, best
    cut = max(best-48, 16)
    return 100.0*sum(hist[:cut])/total, mean, best

path = sys.argv[1]
guard = int(sys.argv[2]) if len(sys.argv) > 2 else 200
w, h, bpp, px = read_pnm(path)
c = autocrop(px, w, h, bpp)
print(f'{path}: {w}x{h}')
if not c:
    print('  autocrop: kein Blatt gefunden'); sys.exit()
x0, y0, x1, y1, page = c
cw, ch = x1-x0+1, y1-y0+1
print(f'  autocrop: x {x0}..{x1} y {y0}..{y1} -> {cw}x{ch}, page white {page}')
sub = bytearray()
for y in range(y0, y1+1):
    sub += px[(y*w+x0)*bpp:(y*w+x1+1)*bpp]
pct, mean, best = ink(bytes(sub), cw, ch, bpp, guard)
print(f'  Wache {guard}: Mittel {mean}, Weiss-Modus {best}, Tinte {pct:.4f} %')
print(f'  -> {"NICHT" if pct > 0.05 else ""} als leer verworfen (Grenze 0.05 %)')
