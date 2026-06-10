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

CELL_W, CELL_H = 8, 16
FG = (0xD5, 0xC4, 0xA1)          # rd_core default face
BG = (0x1D, 0x20, 0x21)


def load_font():
    """Parse font8x8_basic out of src/font8x8.h -- the exact glyphs on screen."""
    src = open("src/font8x8.h").read()
    rows = re.findall(r"\{((?:0x[0-9A-Fa-f]{2},?\s*){8})\}", src)
    font = []
    for r in rows[:128]:
        font.append([int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", r)])
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
    """Decode the character in cell (col,row) by matching fg/bg pixels against
    every glyph; return the best match (or '?' if nothing fits)."""
    best, best_score = "?", -1
    # Extract the cell's 8x8 'is foreground' bitmap (sampling even rows).
    bits = []
    for gy in range(8):
        y = row * CELL_H + gy * 2
        rowbits = 0
        for gx in range(8):
            x = col * CELL_W + gx
            off = 3 * (y * w + x)
            px = data[off:off + 3]
            # fg if closer to FG than BG (any face: just 'not background').
            d_bg = sum(abs(px[i] - BG[i]) for i in range(3))
            d_fg = sum(abs(px[i] - FG[i]) for i in range(3))
            if d_fg < d_bg:
                rowbits |= 1 << gx
        bits.append(rowbits)
    for ch in range(32, 127):
        score = sum(bin(~(bits[i] ^ font[ch][i]) & 0xFF).count("1") for i in range(8))
        if score > best_score:
            best, best_score = chr(ch), score
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
