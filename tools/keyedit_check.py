#!/usr/bin/env python3
"""
keyedit_check.py -- basic Emacs editing keys at the REPL (Phase 27.5).

C-a/C-e move to line ends, C-d deletes forward, C-k kills to end of line, and
M-d kills the word ahead -- the everyday Emacs motion/editing set, working in
the REPL (clamped at the prompt) and file buffers alike. We verify them by
EDITING expressions into shape and checking the evaluated result:

  "ZZ(* 6 7)"   C-a, C-d, C-d  -> "(* 6 7)"  = 42   (C-a + C-d delete forward)
  "junk(+ 8 8)" C-a, M-d       -> "(+ 8 8)"  = 16   (M-d kills the word "junk")
  "garbage"     C-a, C-k       -> ""; type "(+ 1 1)" = 2   (C-k kills the line)

A frame without these keys leaves the junk in place, so the forms error out
instead of producing 42 / 16 / 2.

Run from the repo root:  python3 tools/keyedit_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text


def _chord(mod, letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": mod}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": mod}}}]})
    time.sleep(0.4)


def ctrl(letter): _chord("ctrl", letter)
def meta(letter): _chord("alt", letter)


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-keyedit-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        # M-d uses Alt, whose injected key-state can bleed into the next
        # keystrokes under QMP, so we run the Alt scenario LAST.
        qmp_type("ZZ(* 6 7)"); time.sleep(0.5)     # C-a to start, C-d twice
        ctrl("a"); ctrl("d"); ctrl("d")
        qmp_type("\n"); time.sleep(0.9)

        qmp_type("garbage"); time.sleep(0.5)       # C-a, C-k kills the line
        ctrl("a"); ctrl("k")
        qmp_type("(+ 1 1)\n"); time.sleep(0.9)

        qmp_type("junk(+ 8 8)"); time.sleep(0.5)   # C-a, M-d kills "junk"
        ctrl("a"); meta("d")
        qmp_type("\n"); time.sleep(0.9)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(20)]
        for i, t in enumerate(lines):
            if t:
                print(f"  row {i}: {t!r}")

        ok = True
        for want, what in [("42", "C-a + C-d (delete forward)"),
                           ("16", "M-d (kill word)"),
                           ("2", "C-k (kill line)")]:
            if not any(t == want for t in lines):
                print(f"FAIL: {what} -- result {want} not on screen"); ok = False
        if ok:
            print("PASS: 27.5 basic Emacs editing keys verified")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
