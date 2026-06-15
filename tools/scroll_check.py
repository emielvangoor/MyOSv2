#!/usr/bin/env python3
"""
scroll_check.py -- windows scroll to keep point visible (Phase 27.4).

Emacs keeps point on screen: when output runs past the bottom of a window, the
window's start line advances so the cursor stays visible. MyOSv2 had the
machinery (per-window top_line) but never moved it, so a buffer taller than its
window simply ran off the bottom -- the prompt and latest output vanished.

This prints 50 numbered lines at the REPL (far more than the ~34 text rows of
the window). Afterwards the LATEST output must be visible -- "49", the result
"nil", and a fresh prompt -- and the banner must have scrolled off the top.

Run from the repo root:  python3 tools/scroll_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, GFX_H, CELL_H


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-scroll-check.ppm")
    rows = GFX_H // CELL_H
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        # Print 0..49, one per line -- more lines than the window is tall.
        qmp_type('(let ((i 0)) (while (< i 50) (princ i) (terpri) '
                 '(setq i (+ i 1))))\n')
        time.sleep(2.0)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(rows)]
        for i, t in enumerate(lines):
            if t:
                print(f"  row {i}: {t!r}")

        ok = True
        if lines[0].startswith("MyOSv2 Graphical"):
            print("FAIL: banner still at the top -- window did not scroll"); ok = False
        if not any(t == "49" for t in lines):
            print("FAIL: last printed line (49) is off-screen"); ok = False
        if not any(t.startswith("lisp> ") for t in lines):
            print("FAIL: the post-eval prompt is off-screen"); ok = False
        if ok:
            print("PASS: 27.4 windows scroll to keep point visible")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
