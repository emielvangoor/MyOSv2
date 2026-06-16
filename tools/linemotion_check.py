#!/usr/bin/env python3
"""
linemotion_check.py -- C-p/C-n move by line in a non-REPL buffer (no history leak).

Regression for the separation bug where C-p/C-n (bound to repl history in the
global keymap) spliced REPL history into file/scratch buffers. Now global
C-p/C-n are line motion; repl-mode overrides them with history. In *scratch*
(lisp-interaction), type two lines, C-p up to the first, type Z -> "AAAAZ".
"""
import os, sys, tempfile, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, CELL_H


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-linemotion.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        qmp_type("AAAA\nBBBB"); time.sleep(0.5)   # two lines in *scratch* (RET=newline)
        ctrl("p"); time.sleep(0.4)                # up to the AAAA line (same column)
        qmp_type("Z"); time.sleep(0.4)            # lands on line 1 -> "AAAAZ"
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(h // CELL_H)]
        for i, t in enumerate(lines):
            if t.strip(): print(f"  row {i}: {t!r}")
        ok = any("AAAAZ" in t for t in lines) and any(t.strip() == "BBBB" for t in lines)
        print("PASS: C-p moved by line in a non-REPL buffer (no history leak)" if ok
              else "FAIL: line motion did not work / buffer corrupted")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
