#!/usr/bin/env python3
"""
bufferlist_check.py -- C-x C-b list buffers + C-x 1 full window (Phase 27.9).

Two Emacs staples for managing the windows you accumulate:
  C-x C-b  list every live buffer in a *Help* window (the buffer-list command)
  C-x 1    delete other windows -- the selected window fills the frame again
           (how you get "the full interface" back after splits pile up)

We press C-x C-b and check the buffer list shows up, then C-x 1 and check the
help window is gone (its "Buffers" heading no longer on screen) and the REPL
fills the frame.

Run from the repo root:  python3 tools/bufferlist_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, GFX_H, CELL_H


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})
    time.sleep(0.3)


def flat(font, dump):
    qmp_screendump(dump); time.sleep(0.5)
    w, h, data = read_ppm(dump)
    rows = GFX_H // CELL_H
    return " | ".join(t for t in (row_text(font, w, data, r, 60) for r in range(rows)) if t)


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-bufferlist-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        ok = True

        ctrl("x"); time.sleep(0.2); ctrl("b"); time.sleep(1.0)   # C-x C-b
        f = flat(font, dump)
        print(f"  C-x C-b: {f!r}")
        if not ("Buffers" in f and "repl" in f):
            print("FAIL: C-x C-b did not list buffers"); ok = False

        ctrl("x"); time.sleep(0.2); qmp_type("1"); time.sleep(0.8)  # C-x 1
        f = flat(font, dump)
        print(f"  C-x 1: {f!r}")
        if "Buffers" in f:
            print("FAIL: C-x 1 did not delete the other (help) window"); ok = False
        if "lisp>" not in f:
            print("FAIL: the REPL is not on screen after C-x 1"); ok = False

        if ok:
            print("PASS: 27.9 buffer list (C-x C-b) + full window (C-x 1) verified")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
