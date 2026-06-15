#!/usr/bin/env python3
"""
mxscroll_check.py -- the M-x vertico list scrolls (Phase 27.7).

The minibuffer showed only the first 8 matches and the selection couldn't move
past them, so commands further down were unreachable. Now the 8-row window
scrolls to follow the selection (like a buffer), and the prompt shows a vertico
position counter "i/n".

We open M-x with no filter (every command -- far more than 8 matches) and press
Down 12 times. With scrolling the selection reaches #13, so the prompt reads
"13/..."; without it the selection sticks at 8.

Run from the repo root:  python3 tools/mxscroll_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_screendump
from frame_check import load_font, read_ppm, row_text, GFX_H, CELL_H


def meta_x():
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "alt"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "x"}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "x"}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "alt"}}}]})
    time.sleep(0.4)


def down():
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "down"}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "down"}}}]})
    time.sleep(0.2)


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-mxscroll-check.ppm")
    rows = GFX_H // CELL_H
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        meta_x()                          # M-x, no filter -> all commands
        time.sleep(0.5)
        for _ in range(12):               # move the selection down past the window
            down()
        time.sleep(0.4)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r, 50) for r in range(rows)]
        flat = " | ".join(t for t in lines if t)
        print(f"  echo: {flat!r}")

        if "13/" in flat:
            print("PASS: 27.7 M-x list scrolls (selection reached 13)")
            return 0
        print("FAIL: selection did not scroll past the visible window")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
