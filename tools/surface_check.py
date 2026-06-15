#!/usr/bin/env python3
"""surface_check.py -- the teapot is a surface-mode buffer.

The graphical-buffer arm of the major-mode system: open a REPL with C-x r,
run (teapot) (a TinyGL 3D app that paints into a shared-memory canvas), and
verify the *teapot* window's mode line reads "(Surface)". Surface buffers keep
their shm-canvas mechanics; wrapping their creation in `make-surface` is what
puts them in surface-mode so keys are inert and the mode line names the mode.

Run from the repo root:  python3 tools/surface_check.py
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, inv_row, CELL_H


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-surface-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)   # C-x r: a REPL
        qmp_type("(teapot)\n"); time.sleep(2.5)                       # open *teapot*
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(h // CELL_H)]
        # Mode lines render in the inverse-video face (face 1), which the normal
        # row_text decoder cannot read; OCR every row with the inverse face too.
        # The teapot opens in a split window, so scan all rows for "(Surface)".
        inv = [inv_row(font, w, data, r) for r in range(h // CELL_H)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}   inv: {inv[i]!r}")
        ml = "".join(inv)
        ok = "(Surface)" in ml
        print("PASS: *teapot* is a surface-mode buffer" if ok
              else "FAIL: (Surface) mode line not seen")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
