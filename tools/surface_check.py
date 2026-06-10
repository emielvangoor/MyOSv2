#!/usr/bin/env python3
"""
surface_check.py -- end-to-end check for Phase 25.6 (surface buffers).

The EXWM move, verified by screendump: a surface buffer appears in the window
tree like any text buffer; first Lisp paints it (surface-fill-rect), then an
EXTERNAL program (run-in-buffer -> /bin/surftest, drawing into shared memory)
takes it over, and its pixels appear inside the Emacs-style frame.

Run from the repo root:  python3 tools/surface_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp_type, qmp_screendump
from frame_check import read_ppm

# 1280x720 frame at 12x24 cells: 30 rows, 29 for windows. split-below ->
# a gets 14 rows, the bottom window starts at cell row 14 -> pixel y 336.
SURF_Y = 14 * 24


def px(data, w, x, y):
    off = 3 * (y * w + x)
    return tuple(data[off:off + 3])


def main() -> int:
    dump = os.path.join(tempfile.gettempdir(), "myosv2-surface-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        qmp_type("(split-below)\n")
        time.sleep(0.5)
        # One atomic form: select the bottom window, give it a fresh surface
        # buffer, come back -- so the next keystrokes still hit the REPL.
        qmp_type('(progn (other-window) (set-buffer (setq sb '
                 '(make-surface-buffer "*surf*" 400 160))) (other-window))\n')
        time.sleep(1.0)
        qmp_type("(surface-fill-rect sb 0 0 400 160 255)\n")   # blue
        time.sleep(1.0)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        got = px(data, w, 200, SURF_Y + 80)
        if not (got[2] > 200 and got[0] < 60):
            print(f"FAIL: Lisp-drawn surface not on screen at (200,{SURF_Y+80}): {got}")
            return 1
        print("ok: (surface-fill-rect ...) pixels inside the frame")

        # Now the external renderer takes the canvas over.
        qmp_type('(run-in-buffer sb "surftest")\n')
        time.sleep(2.5)
        qmp_type("(+ 0 0)\n")                  # any eval -> redisplay -> blit
        time.sleep(1.0)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        checks = [
            ((200, SURF_Y + 80), (0x00, 0xCC, 0x44), "green field"),
            ((1,   SURF_Y + 1),  (0xFF, 0xFF, 0xFF), "white frame"),
            ((30,  SURF_Y + 30), (0xFF, 0x00, 0xFF), "magenta square"),
        ]
        for (x, y), want, label in checks:
            got = px(data, w, x, y)
            if any(abs(a - b) > 40 for a, b in zip(got, want)):
                print(f"FAIL: surftest {label} at ({x},{y}): want ~{want}, got {got}")
                return 1
            print(f"ok: surftest {label} on screen")

        print("PASS: 25.6 surface buffers verified (Lisp + external renderer)")
        return 0
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
