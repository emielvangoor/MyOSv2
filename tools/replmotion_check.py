#!/usr/bin/env python3
"""
replmotion_check.py -- point motion at the REPL prompt (Phase 27.2 fix).

C-b/C-f (and the Left/Right arrows that cook to them) must move the cursor
WITHIN the current REPL input, not just in file buffers. We type "12", press
Left once (point lands between the 1 and the 2), type "0", and evaluate: with
motion the line reads "102" and evaluates to 102; without it the "0" appends
and you get "120". So a result row of "102" proves the cursor moved.

Run from the repo root:  python3 tools/replmotion_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp_key, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, ctrl


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-replmotion-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)  # C-x r: REPL in this window

        qmp_type("12"); time.sleep(0.3)
        qmp_key("left"); time.sleep(0.3)      # cursor between 1 and 2
        qmp_type("0"); time.sleep(0.3)        # -> "102" if motion worked
        qmp_type("\n"); time.sleep(0.6)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(12)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}")

        if any(t.startswith("lisp> 12") and "0" in t for t in lines) and \
           any(t == "120" for t in lines):
            print("FAIL: Left did not move point (got 120 -- '0' appended)")
            return 1
        if any(t == "102" for t in lines):
            print("PASS: REPL cursor motion verified (Left inserted mid-line)")
            return 0
        print("FAIL: did not see the expected 102 result")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
