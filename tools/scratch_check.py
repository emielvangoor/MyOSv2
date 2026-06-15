#!/usr/bin/env python3
"""scratch_check.py -- the frame boots into *scratch* (lisp-interaction-mode)."""
import os, sys, tempfile, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp_screendump
from frame_check import load_font, read_ppm, row_text, inv_row, CELL_H, CELL_W


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-scratch-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(h // CELL_H)]
        # Mode lines are inverse-video, so OCR every row with the inverse face
        # too; one of these rows holds "-- *scratch*  (Lisp Interaction) --".
        inv = [inv_row(font, w, data, r) for r in range(h // CELL_H)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}   inv: {inv[i]!r}")
        # The mode line names the mode; the scratch banner is present.
        ml = "".join(inv)
        ok_mode = "(Lisp Interaction)" in ml
        ok_banner = any("scratch" in t for t in lines)
        if ok_mode and ok_banner:
            print("PASS: boots into *scratch* (Lisp Interaction)")
            return 0
        print(f"FAIL: mode={ok_mode} banner={ok_banner}")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
