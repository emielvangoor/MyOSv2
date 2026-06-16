#!/usr/bin/env python3
"""
bufswitch_check.py -- C-x b switch-to-buffer with completion (Phase 27.10).

C-x b reads a buffer name in the vertico minibuffer (completing over the live
buffers) and shows the chosen buffer in the current window. We give a file
buffer some content, hide it (C-x 1 leaves only the REPL), then C-x b back to
it and check its content reappears.

  (find-file "/sw.txt")  C-x o   type HELLO-SWITCH   C-x o   C-x 1
  C-x b  sw  RET   ->   the window now shows /sw.txt with HELLO-SWITCH

Run from the repo root:  python3 tools/bufswitch_check.py
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
    dump = os.path.join(tempfile.gettempdir(), "myosv2-bufswitch-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        # Open the file the Emacs way -- interactive C-x C-f opens it in the
        # CURRENT window and the selection persists (unlike a (find-file-here)
        # typed at the REPL, whose eval loop restores its own buffer). Then type
        # content, switch the window AWAY to *scratch* (hiding /sw.txt), and
        # finally C-x b back to it.
        ctrl("x"); time.sleep(0.2); ctrl("f"); time.sleep(0.5)      # C-x C-f
        qmp_type("/sw.txt\n"); time.sleep(0.8)                       # window -> /sw.txt
        qmp_type("HELLO-SWITCH"); time.sleep(0.5)                    # content
        ctrl("x"); time.sleep(0.2); qmp_type("b"); time.sleep(0.5)   # C-x b
        qmp_type("scratch"); time.sleep(0.6)                         # filter to *scratch*
        qmp_type("\n"); time.sleep(0.8)                              # -> the REPL, /sw.txt hidden

        # Now switch the (single) window back to /sw.txt by name.
        ctrl("x"); time.sleep(0.2); qmp_type("b"); time.sleep(0.5)   # C-x b
        qmp_type("sw"); time.sleep(0.6)                              # filter
        qmp_type("\n"); time.sleep(0.8)                             # commit

        f = flat(font, dump)
        print(f"  after C-x b sw: {f!r}")
        ok = True
        if "HELLO-SWITCH" not in f:
            print("FAIL: switching did not show /sw.txt's content"); ok = False
        if "lisp>" in f:
            print("FAIL: still showing the REPL -- switch did not happen"); ok = False
        if ok:
            print("PASS: 27.10 C-x b switch-to-buffer verified")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
