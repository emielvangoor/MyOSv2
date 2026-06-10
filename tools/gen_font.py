#!/usr/bin/env python3
"""
gen_font.py -- prerender an anti-aliased bitmap font for rd_core.

Rasterizes ASCII 32..126 from a TTF into fixed-size cells of 8-bit alpha
(coverage) values and emits src/font_aa.h. The OS then renders text by
integer alpha blending -- out = bg + (fg-bg)*a/255 -- which is ordinary
grayscale font anti-aliasing, done with no floating point at runtime.

The generated header is COMMITTED, so normal builds never need this script
(or the font file, or PIL). Re-run only to change font or cell size:

    python3 -m venv /tmp/fontgen-venv
    /tmp/fontgen-venv/bin/pip install pillow
    /tmp/fontgen-venv/bin/python tools/gen_font.py \
        ~/Library/Fonts/EmielPro-Regular.ttf src/font_aa.h
"""

import sys

from PIL import Image, ImageDraw, ImageFont

CELL_W, CELL_H = 12, 24


def pick_size(path):
    """Largest point size whose widest ASCII advance fits CELL_W and whose
    ascent+descent fits CELL_H."""
    best = None
    for size in range(8, 49):
        f = ImageFont.truetype(path, size)
        ascent, descent = f.getmetrics()
        widest = max(f.getlength(chr(c)) for c in range(32, 127))
        if widest <= CELL_W and ascent + descent <= CELL_H:
            best = (size, f, ascent, descent)
    if not best:
        raise SystemExit("no size fits the cell")
    return best


def main():
    font_path, out_path = sys.argv[1], sys.argv[2]
    size, font, ascent, descent = pick_size(font_path)
    name = "/".join(font.getname())
    print(f"{name} at {size}pt (ascent {ascent}, descent {descent}) "
          f"into {CELL_W}x{CELL_H} cells")

    # Vertical centering: park the baseline so ascent+descent sit centered.
    y_off = (CELL_H - (ascent + descent)) // 2

    glyphs = []
    for c in range(32, 127):
        img = Image.new("L", (CELL_W, CELL_H), 0)
        d = ImageDraw.Draw(img)
        w = font.getlength(chr(c))
        x_off = (CELL_W - w) / 2          # center each glyph in its cell
        d.text((x_off, y_off), chr(c), font=font, fill=255)
        glyphs.append(bytes(img.getdata()))

    with open(out_path, "w") as f:
        f.write(f"""// font_aa.h -- prerendered anti-aliased glyphs for rd_core.
// =========================================================
// {name} at {size}pt, rasterized into {CELL_W}x{CELL_H} cells of 8-bit ALPHA
// (coverage) by tools/gen_font.py. The renderer blends each pixel with
// integer math -- out = bg + (fg-bg)*a/255 -- so the OS gets genuinely
// anti-aliased text with no floating point and no runtime rasterizer.
// ASCII 32..126; index with font_aa[ch - 32][y * {CELL_W} + x].
#pragma once
#include <stdint.h>

#define FONT_AA_W {CELL_W}
#define FONT_AA_H {CELL_H}
#define FONT_AA_FIRST 32
#define FONT_AA_LAST 126

static const uint8_t font_aa[95][{CELL_W * CELL_H}] = {{
""")
        for i, g in enumerate(glyphs):
            f.write(f"    {{ // {chr(32 + i)!r}\n")
            for row in range(CELL_H):
                vals = ",".join(f"{g[row * CELL_W + x]:3d}" for x in range(CELL_W))
                f.write(f"        {vals},\n")
            f.write("    },\n")
        f.write("};\n")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
