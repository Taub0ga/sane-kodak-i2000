#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
"""Measure vertical streaking in a PGM/PNM: column means over the inner area.

Usage:  column-uniformity.py scan.pgm [inset]

Prints the figures NATIVES-BACKEND.md section 17 quotes for a blank sheet:
mean, range, mean deviation, neighbour jump and the number of columns more
than 12 levels below the mean.  `inset` is the fraction of each edge to leave
out (default 0.10) so the dark transport border stays out of the numbers.
"""
import sys

def read_pnm(path):
    d = open(path, 'rb').read()
    tok, i = [], 0
    while len(tok) < 4:
        while i < len(d) and d[i:i+1].isspace(): i += 1
        if d[i:i+1] == b'#':
            while d[i:i+1] != b'\n': i += 1
            continue
        j = i
        while j < len(d) and not d[j:j+1].isspace(): j += 1
        tok.append(d[i:j]); i = j
    i += 1
    magic, w, h, mx = tok[0], int(tok[1]), int(tok[2]), int(tok[3])
    bpp = 3 if magic == b'P6' else 1
    return w, h, bpp, d[i:i + w*h*bpp]

def main(path, inset=0.10):
    w, h, bpp, px = read_pnm(path)
    x0, x1 = int(w*inset), int(w*(1-inset))
    y0, y1 = int(h*inset), int(h*(1-inset))
    n = y1 - y0
    cols = []
    for x in range(x0, x1):
        s = 0
        for y in range(y0, y1):
            s += px[(y*w + x)*bpp]
        cols.append(s/n)
    mean = sum(cols)/len(cols)
    dev = sum(abs(c-mean) for c in cols)/len(cols)
    jump = sum(abs(cols[i]-cols[i-1]) for i in range(1, len(cols)))/(len(cols)-1)
    dark = [i+x0 for i, c in enumerate(cols) if c < mean-12]
    print(f"{path}: {w}x{h} bpp={bpp}  inner x {x0}..{x1} y {y0}..{y1}")
    print(f"  Mittel {mean:.1f}   Spanne {min(cols):.1f}..{max(cols):.1f}")
    print(f"  mittlere Abweichung {dev:.2f}   Nachbarsprung {jump:.2f}")
    print(f"  Spalten mehr als 12 unter dem Mittel: {len(dark)}")
    if dark:
        print(f"  erste dunkle Spalten: {dark[:20]}")

if __name__ == '__main__':
    main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.10)
