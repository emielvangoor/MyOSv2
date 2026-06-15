#!/usr/bin/env python3
"""
helpkeys_check.py -- self-documenting help: C-h b / C-h k / C-h m (Phase 27.8).

Emacs is famously self-documenting: the keymaps are data, and C-h reads the
same data the dispatcher uses. We verify the three classics:

  C-h b   list all key bindings (a *Help* window of "C-a ...", "C-x 2 ...", ...)
  C-h k   read a key, report the command it runs (C-h k C-d -> delete forward)
  C-h m   describe the current mode (REPL) and its bindings

Run from the repo root:  python3 tools/helpkeys_check.py
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
    dump = os.path.join(tempfile.gettempdir(), "myosv2-helpkeys-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        ok = True

        # C-h b -- the bindings list.
        ctrl("h"); time.sleep(0.2); qmp_type("b"); time.sleep(1.0)
        f = flat(font, dump)
        print(f"  C-h b: {f!r}")
        if not ("C-a" in f and "C-x 2" in f and "M-d" in f):
            print("FAIL: C-h b did not list the bindings"); ok = False

        # C-h k C-d -- describe one key.
        ctrl("h"); time.sleep(0.2); qmp_type("k"); time.sleep(0.5)
        ctrl("d"); time.sleep(1.0)
        f = flat(font, dump)
        print(f"  C-h k C-d: {f!r}")
        if "delete-forward" not in f:
            print("FAIL: C-h k did not name the command bound to C-d"); ok = False

        # C-h m -- describe the mode.
        ctrl("h"); time.sleep(0.2); qmp_type("m"); time.sleep(1.0)
        f = flat(font, dump)
        print(f"  C-h m: {f!r}")
        if "REPL" not in f:
            print("FAIL: C-h m did not describe the REPL mode"); ok = False

        if ok:
            print("PASS: 27.8 self-documenting help (C-h b/k/m) verified")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
