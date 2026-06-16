#!/usr/bin/env python3
"""replmode_check.py -- C-x r opens a repl-mode window whose RET evaluates."""
import os, sys, tempfile, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, inv_row, CELL_H, CELL_W


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-replmode-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)   # C-x r
        qmp_type("(+ 2 3)\n"); time.sleep(0.8)                        # RET evaluates
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(h // CELL_H)]
        # Mode lines are inverse-video, so OCR every row with the inverse face
        # too; one of these rows holds "-- *repl*  (REPL) --".
        inv = [inv_row(font, w, data, r) for r in range(h // CELL_H)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}   inv: {inv[i]!r}")
        ml = "".join(inv)
        ok = "(REPL)" in ml and any(t.strip() == "5" for t in lines)
        print("PASS: C-x r REPL evaluates (RET)" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
