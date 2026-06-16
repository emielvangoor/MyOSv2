#!/usr/bin/env python3
"""linewrap_check.py -- M-x line-wrap-mode adds the `Wrap` lighter to the mode line.

Boots the frame, opens M-x (Alt+x), types `line-wrap` (which uniquely narrows to
the single command `line-wrap-mode`), RETs to run it, then screendumps and asserts
the inverse-video mode line now carries the `Wrap` minor-mode lighter.

Run from the repo root:  python3 tools/linewrap_check.py
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, inv_row, CELL_H


def meta_x():
    """Press Alt+x to open M-x (same chord helper mx_check/scratch_check use)."""
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "alt"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "x"}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "x"}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "alt"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-linewrap-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        # M-x line-wrap RET -- "line-wrap" uniquely narrows to line-wrap-mode
        # (the only interactive command whose name contains that fragment),
        # so the highlighted selection is the toggle we want.
        meta_x()
        time.sleep(0.5)
        qmp_type("line-wrap")
        time.sleep(1.5)
        qmp_type("\n")
        time.sleep(1.5)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        # The mode line is inverse-video, so OCR every row with the inverse face.
        inv = [inv_row(font, w, data, r) for r in range(h // CELL_H)]
        body = [row_text(font, w, data, r) for r in range(h // CELL_H)]
        for i, t in enumerate(inv):
            print(f"  row {i}: {body[i]!r}   inv: {t!r}")

        ml = "".join(inv)
        if "Wrap" in ml:
            print("PASS: line-wrap-mode adds the `Wrap` lighter to the mode line")
            return 0
        print(f"FAIL: `Wrap` lighter not on the mode line: {ml!r}")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
