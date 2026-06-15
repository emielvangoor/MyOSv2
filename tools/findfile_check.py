#!/usr/bin/env python3
"""
findfile_check.py -- interactive C-x C-f (Phase 27 modes update).

C-x C-f prompts for a path, opens it in text-mode in the CURRENT window; we
type text, C-x C-s to save, then read it back from disk via a fresh REPL
window (C-x r) and (cat ...). The saved text must stream into the REPL.
"""
import os, sys, tempfile, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
# Body text (the saved file contents read back) is face-0, so row_text reads
# it; iterate rows by CELL_H (the real glyph height), not a hardcoded count.
from frame_check import load_font, read_ppm, row_text, CELL_H


def qmp_ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-findfile-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        qmp_ctrl("x"); time.sleep(0.2); qmp_ctrl("f"); time.sleep(0.5)  # C-x C-f
        qmp_type("/n.txt\n"); time.sleep(0.8)         # path -> opens in this window
        qmp_type("hello edit\n"); time.sleep(0.6)     # text-mode: real text entry
        qmp_ctrl("x"); time.sleep(0.2); qmp_ctrl("s"); time.sleep(0.8)  # save
        qmp_ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)  # C-x r: REPL
        qmp_type('(cat "/n.txt")\n'); time.sleep(1.0)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(h // CELL_H)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}")
        ok = any("hello edit" in t for t in lines)
        print("PASS: C-x C-f edit + save persisted (read back)" if ok
              else "FAIL: saved text did not read back")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
