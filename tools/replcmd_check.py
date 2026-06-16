#!/usr/bin/env python3
"""
replcmd_check.py -- M-x repl opens a working REPL without hanging.

Regression guard for the bug where M-x repl (or evaluating (repl)) fell through
to system.l's blocking demo `repl` -- an infinite (read) loop that froze the
frame's event loop. The frame now defines its own `repl` command (open a fresh
repl-mode window). This drives M-x repl, then evaluates (+ 4 5) in the new REPL
and checks 9 appears -- proving the frame did NOT hang and the REPL works.

Run from the repo root:  python3 tools/replcmd_check.py
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, CELL_H


def meta_x():
    # Alt/Meta + x opens M-x.
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "alt"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "x"}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "x"}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "alt"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-replcmd-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        meta_x(); time.sleep(0.3)
        qmp_type("repl"); time.sleep(0.4)     # narrow to the repl command
        qmp_type("\n"); time.sleep(0.8)        # run it -> a fresh REPL window
        # If the old blocking `repl` ran, the frame is now frozen and the next
        # input never takes effect -> the 9 below never appears (timeout = FAIL).
        qmp_type("(+ 4 5)\n"); time.sleep(0.8)  # repl-mode: RET evaluates

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(h // CELL_H)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}")
        ok = any(t.strip() == "9" for t in lines) and any(t.startswith("lisp>") for t in lines)
        print("PASS: M-x repl opens a working REPL (no hang)" if ok
              else "FAIL: no 9 / no prompt -- frame may have hung")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
