#!/usr/bin/env python3
"""
gen_cursor.py -- prerender an anti-aliased arrow cursor for the virtio-gpu
cursor plane. Draws the classic arrow polygon at 8x supersampling (black
outline, white fill), downscales with Lanczos for smooth alpha edges, and
emits src/cursor_aa.h (64x64 B8G8R8A8). Also writes a preview PNG.

    /tmp/fontgen-venv/bin/python tools/gen_cursor.py src/cursor_aa.h /tmp/cursor.png
"""

import sys

from PIL import Image, ImageDraw

# The classic lean arrow (macOS proportions), in a 0..1 box: tip top-left,
# straight left edge, diagonal shoulder, notched tail.
ARROW = [(0.00, 0.00), (0.00, 0.857), (0.324, 0.679), (0.529, 1.000),
         (0.765, 0.939), (0.559, 0.625), (1.000, 0.625)]

W, H = 34, 56          # on-screen size (within the 64x64 plane); 2x for HiDPI
SS = 8                 # supersampling factor


def main():
    out_path, png_path = sys.argv[1], sys.argv[2]
    img = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pts = [(2 * SS + x * (W - 4) * SS, 2 * SS + y * (H - 4) * SS)
           for x, y in ARROW]
    # White body first, then a thin black stroke ON TOP -- the outline must
    # survive, it is what keeps the pointer visible over light content.
    d.polygon(pts, fill=(255, 255, 255, 255))
    d.line(pts + [pts[0]], fill=(0, 0, 0, 255), width=int(1.4 * SS),
           joint="curve")
    small = img.resize((W, H), Image.LANCZOS)

    plane = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    plane.paste(small, (0, 0))
    plane.save(png_path)

    px = list(plane.getdata())
    with open(out_path, "w") as f:
        f.write("""// cursor_aa.h -- prerendered anti-aliased arrow cursor (tools/gen_cursor.py).
// 64x64 B8G8R8A8 (little-endian word 0xAARRGGBB) for the virtio-gpu cursor
// plane: smooth alpha edges, matching the anti-aliased text it floats over.
#pragma once
#include <stdint.h>

static const uint32_t cursor_aa[64 * 64] = {
""")
        for y in range(64):
            vals = []
            for x in range(64):
                r, g, b, a = px[y * 64 + x]
                vals.append(f"0x{a:02X}{r:02X}{g:02X}{b:02X}")
            f.write("    " + ",".join(vals) + ",\n")
        f.write("};\n")
    print(f"wrote {out_path} + {png_path}")


if __name__ == "__main__":
    main()
