#!/usr/bin/env python3
"""
frame_check.py -- end-to-end check for Phase 25.4 (the graphical Lisp machine).

Boots, starts `lisp -frame` from the serial REPL, then plays a human: types
`(+ 1 2)` + Enter on the QMP keyboard, takes a screendump, and verifies the
GLYPHS -- cell-by-cell against the same 8x8 font the guest renders with -- so
"the screen shows lisp> (+ 1 2) and the result 3" is checked literally.

Run from the repo root:  python3 tools/frame_check.py
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp_type, qmp_screendump

import re as _re
_hdr = open("src/font_aa.h").read()
CELL_W = int(_re.search(r"#define FONT_AA_W (\d+)", _hdr).group(1))
CELL_H = int(_re.search(r"#define FONT_AA_H (\d+)", _hdr).group(1))
FG = (0xD5, 0xC4, 0xA1)          # rd_core default face
BG = (0x1D, 0x20, 0x21)


def load_font():
    """Parse the anti-aliased font out of src/font_aa.h into thresholded
    bitmaps (12 bits per row, 24 rows) -- the exact glyphs on screen."""
    src = open("src/font_aa.h").read()
    body = src[src.index("font_aa[95]"):]
    glyphs = re.findall(r"\{ //[^\n]*\n((?:[^}]|\n)*?)\},", body)
    font = {}
    for i, g in enumerate(glyphs[:95]):
        vals = [int(v) for v in re.findall(r"\d+", g)]
        rows = []
        for y in range(CELL_H):
            bits = 0
            for x in range(CELL_W):
                if vals[y * CELL_W + x] > 127:
                    bits |= 1 << x
            rows.append(bits)
        font[chr(32 + i)] = rows
    return font


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        f.readline()
        data = f.read(w * h * 3)
    return w, h, data


def cell_char(font, w, data, col, row):
    """Decode the character in cell (col,row): threshold each pixel to
    'closer to fg than bg', then find the glyph bitmap with the fewest
    mismatching pixels."""
    bits = []
    for gy in range(CELL_H):
        y = row * CELL_H + gy
        rowbits = 0
        for gx in range(CELL_W):
            x = col * CELL_W + gx
            off = 3 * (y * w + x)
            px = data[off:off + 3]
            d_bg = sum(abs(px[i] - BG[i]) for i in range(3))
            d_fg = sum(abs(px[i] - FG[i]) for i in range(3))
            if d_fg < d_bg:
                rowbits |= 1 << gx
        bits.append(rowbits)
    best, best_score = "?", -1
    mask = (1 << CELL_W) - 1
    for ch, rows in font.items():
        score = sum(bin(~(bits[i] ^ rows[i]) & mask).count("1") for i in range(CELL_H))
        if score > best_score:
            best, best_score = ch, score
    return best


def row_text(font, w, data, row, ncols=40):
    return "".join(cell_char(font, w, data, c, row) for c in range(ncols)).rstrip()


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-frame-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame.l did not load"); return 1
        time.sleep(1.0)

        # Type at the graphical REPL like a human would.
        qmp_type("(+ 1 2)\n")
        time.sleep(1.0)
        # The machine photographs itself: the in-OS screenshot primitive
        # writes a PPM into the ramfs; its `t` on screen proves the write.
        # 2.7 MB through vfs_write is slow under TCG -- poll until it lands.
        qmp_type('(screenshot "/shot.ppm")\n')
        lines = []
        for _ in range(30):
            time.sleep(3.0)
            qmp_screendump(dump)
            time.sleep(0.5)
            w, h, data = read_ppm(dump)
            if (w, h) != (1280, 720):
                print(f"FAIL: unexpected scanout {w}x{h}"); return 1
            lines = [row_text(font, w, data, r) for r in range(10)]
            if any(t == "t" for t in lines):
                break
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}")

        ok = True
        if not lines[0].startswith("MyOSv2 Graphical Lisp Machine"):
            print("FAIL: banner missing"); ok = False
        prompt_rows = [t for t in lines if t.startswith("lisp> ")]
        if not any(t.startswith("lisp> (+ 1 2)") for t in lines):
            print("FAIL: typed input not on screen"); ok = False
        if not any(t == "3" for t in lines):
            print("FAIL: eval result '3' not on screen"); ok = False
        if not any(t == "t" for t in lines):
            print("FAIL: (screenshot) did not return t on screen"); ok = False
        if len(prompt_rows) < 2:
            print("FAIL: no fresh prompt after eval"); ok = False
        if ok:
            print("PASS: 25.4 graphical Lisp machine verified (glyph-level)")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
