#!/usr/bin/env python3
"""
split_check.py -- C-x 2 gives the new window its OWN fresh REPL (Phase 27.6).

Before: C-x 2 split the window but both halves showed the SAME buffer, so the
new window just mirrored the old one (typing in either changed both). The Emacs
Lisp-machine behavior we want: each split is an independent REPL with its own
prompt and input. We split, evaluate (+ 2 2) in the NEW window, and check that
its result appears there -- and that the form did NOT bleed into the original
(top) window, which would prove they still share one buffer.

Run from the repo root:  python3 tools/split_check.py
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


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-split-check.ppm")
    rows = GFX_H // CELL_H
    top_half = rows // 2          # rows belonging to the original (top) window
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)  # C-x r: REPL in this window

        ctrl("x"); time.sleep(0.2); qmp_type("2"); time.sleep(0.6)  # C-x 2 split
        qmp_type("(+ 2 2)\n"); time.sleep(0.8)                      # eval in new REPL

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(rows)]
        for i, t in enumerate(lines):
            if t:
                print(f"  row {i}: {t!r}")

        ok = True
        if not any(t == "4" for t in lines):
            print("FAIL: (+ 2 2) did not evaluate to 4 anywhere"); ok = False
        # The form must live in the bottom (new) window only -- not mirrored up.
        if any("(+ 2 2)" in lines[r] for r in range(top_half - 1)):
            print("FAIL: the form appeared in the TOP window -- still mirroring")
            ok = False
        # Two independent prompts (the original + the fresh one).
        if len([t for t in lines if t.startswith("lisp>")]) < 2:
            print("FAIL: expected a fresh prompt in the new window"); ok = False
        if ok:
            print("PASS: 27.6 split spawns an independent REPL")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
