#!/usr/bin/env python3
"""Render a line or two of text into the i2600 panel's bitmap format.

The operator panel is a 128x48 graphical LCD in the usual page layout: six
pages of eight stacked pixel rows, one byte per column within a page, bit 0
topmost.  The vendor driver renders text with Pango and Cairo; this does the
same job with PIL, which is already a dependency of the analysis tools.

  tools/lcd-text.py out.bin "Bereit"
  tools/lcd-text.py out.bin "Scanne" "Seite 3"
  tools/lcd-text.py --reserve-top 6 out.bin "Papierstau"
  native/i2600-lcd out.bin

--reserve-top keeps the top N rows empty.  Region 4 needs it: the device
draws its own status letter and screen number up there, and the vendor's
own region 4 bitmaps start at row 6.  Region 2 needs nothing reserved --
the status corner is blank while a region 2 message is shown.
"""
import sys
from PIL import Image, ImageDraw, ImageFont

W, H, PAGES = 128, 48, 6

FONT_CANDIDATES = [
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSans-Bold.ttf",
]


def pick_font(size):
    for p in FONT_CANDIDATES:
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    return ImageFont.load_default()


def render(lines, reserve_top=0):
    im = Image.new("1", (W, H), 0)
    d = ImageDraw.Draw(im)
    n = max(1, len(lines))
    avail = H - reserve_top
    size = 22 if n == 1 else 16
    if size * n > avail:
        size = max(7, avail // n - 1)
    for i, text in enumerate(lines):
        f = pick_font(size)
        # Shrink until it fits the width.
        while size > 7:
            box = d.textbbox((0, 0), text, font=f)
            if box[2] - box[0] <= W - 2:
                break
            size -= 1
            f = pick_font(size)
        box = d.textbbox((0, 0), text, font=f)
        x = max(0, (W - (box[2] - box[0])) // 2 - box[0])
        y = reserve_top + int((avail / n) * i
                              + (avail / n - (box[3] - box[1])) / 2) - box[1]
        d.text((x, y), text, font=f, fill=1)
    return im


def to_pages(im):
    px = im.load()
    out = bytearray(PAGES * W)
    for page in range(PAGES):
        for x in range(W):
            b = 0
            for bit in range(8):
                y = page * 8 + bit
                if y < H and px[x, y]:
                    b |= 1 << bit
            out[page * W + x] = b
    return bytes(out)


def main():
    argv = sys.argv[1:]
    reserve_top = 0
    if argv and argv[0] == "--reserve-top":
        reserve_top = int(argv[1])
        argv = argv[2:]
    if len(argv) < 2:
        sys.exit(__doc__)
    out, lines = argv[0], argv[1:]
    im = render(lines, reserve_top)
    data = to_pages(im)
    assert len(data) == PAGES * W
    with open(out, "wb") as fh:
        fh.write(data)
    # Show what will appear, so a wrong font or size is obvious before upload.
    px = im.load()
    for y in range(H):
        print("".join("#" if px[x, y] else "." for x in range(W)))
    print(f"{out}: {len(data)} bytes")


if __name__ == "__main__":
    main()
