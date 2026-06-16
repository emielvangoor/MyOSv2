#!/usr/bin/env python3
"""
history_check.py -- REPL input history in the frame (Phase 27.2).

A shell remembers what you typed; so should the Lisp machine's REPL. Arrow
Up/Down (which the kernel cooks into C-p/C-n) walk a history of submitted
forms, dropping the recalled text back at the prompt ready to re-run or edit.

The test evaluates (+ 1 2) then (* 2 5), presses Up TWICE to walk back to
(+ 1 2), and hits RET -- the recalled form must reappear at the prompt and
evaluate to 3 a second time.

Run from the repo root:  python3 tools/history_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text


def qmp_arrow(which):
    """Press an arrow key (cooked into C-p / C-n by the guest)."""
    qmp("input-send-event", {"events": [{"type": "key",
        "data": {"down": True, "key": {"type": "qcode", "data": which}}}]})
    qmp("input-send-event", {"events": [{"type": "key",
        "data": {"down": False, "key": {"type": "qcode", "data": which}}}]})


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-history-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)  # C-x r: REPL in this window

        qmp_type("(+ 1 2)\n"); time.sleep(0.6)
        qmp_type("(* 2 5)\n"); time.sleep(0.6)
        qmp_arrow("up"); time.sleep(0.4)      # recalls (* 2 5)
        qmp_arrow("up"); time.sleep(0.4)      # recalls (+ 1 2)
        qmp_type("\n"); time.sleep(0.6)       # evaluate the recalled form

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(16)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}")

        # The recalled form must have been re-submitted at a prompt...
        recalled = [t for t in lines if t.startswith('lisp> (+ 1 2)')]
        threes = [t for t in lines if t == "3"]
        ok = True
        if len(recalled) < 2:
            print("FAIL: Up did not recall (+ 1 2) to a second prompt"); ok = False
        if len(threes) < 2:
            print("FAIL: recalled form did not evaluate to 3 again"); ok = False
        if ok:
            print("PASS: 27.2 REPL input history verified")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
