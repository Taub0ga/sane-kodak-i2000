#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Thomas Parthe
"""Read the panel bitmaps back out of a capture, as text.

lcd-text.py renders text into the 768-byte panel format; this does the
reverse for the vendor's own uploads, so one can see which message the
driver stored in which screen.  That is how screen 1 was identified as
"Rescan documents" and screen 9 as "Paper jam".

Decoding is by eye, not OCR: the glyphs are printed one per block, split
at the gaps between columns, which is enough to read them off reliably.
Reading the whole 128x48 grid at once is not -- P and R differ only in
whether the stem continues below the bowl, and that is easy to misread.

Word gaps are wider than letter gaps, but not by much: inside "Paper"
the gaps run 2-3 columns, between "Paper" and "jam" it is 5.  So the
threshold is 5, and anything at or above it is marked.

Usage: tools/lcd-decode.py <capture.pcapng>
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                'protocol'))
import ctrl

OP_OCP_BLOCK = 0x62
PANEL_INDEX  = 0x0680
PANEL_BYTES  = 768
WORD_GAP     = 5


def grid(b):
    """The 768 bytes as 48 rows of 128, 6 pages of 128 columns, bit 0 top."""
    return [[1 if b[(y // 8) * 128 + x] & (1 << (y % 8)) else 0
             for x in range(128)] for y in range(48)]


def glyphs(g, y0, y1):
    """Group non-empty columns into glyphs; yield (columns, gap following)."""
    out, cur, prev = [], [], None
    for x in range(128):
        if not any(g[y][x] for y in range(y0, y1 + 1)):
            continue
        if prev is not None and x - prev > 1:
            out.append((cur, x - prev))
            cur = []
        cur.append(x)
        prev = x
    if cur:
        out.append((cur, 0))
    return out


def main(path):
    v = [t for t in ctrl.transfers(path) if (t['bmr'] & 0x60) == 0x40]
    uploads = [t for t in v if t['breq'] == OP_OCP_BLOCK
               and t['widx'] == PANEL_INDEX and len(t['data']) == PANEL_BYTES]
    if not uploads:
        sys.exit('no panel uploads in %s' % os.path.basename(path))

    for wval in sorted({t['wval'] for t in uploads}):
        t = next(t for t in uploads if t['wval'] == wval)
        g = grid(t['data'])
        rows = [y for y in range(48) if any(g[y])]
        if not rows:
            print('screen %d (0x%04x): blank\n' % (wval >> 8, wval))
            continue
        y0, y1 = min(rows), max(rows)
        n = sum(1 for t2 in uploads if t2['wval'] == wval)
        print('==== screen %d, region %d (wValue 0x%04x), written %dx, rows %d-%d'
              % (wval >> 8, wval & 0xff, wval, n, y0, y1))
        for cols, gap in glyphs(g, y0, y1):
            for y in range(y0, y1 + 1):
                print('   ' + ''.join('#' if g[y][x] else '.' for x in cols))
            print('   cols %d-%d%s' % (cols[0], cols[-1],
                  '   <- word gap' if gap >= WORD_GAP else ''))
        print()


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    main(sys.argv[1])
