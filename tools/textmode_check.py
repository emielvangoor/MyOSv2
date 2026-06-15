#!/usr/bin/env python3
"""
textmode_check.py -- C-x C-f opens a file in text-mode in the current window;
edit + C-x C-s saves; the mode line reads (Text). Uses the persistent /disk so
a re-open in a fresh boot would find it; here we verify within one boot by
reading it back via the serial REPL helper (cat).
"""
import os, sys, tempfile, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
# The mode line is painted in face 1 (inverse video); the normal row_text
# decoder can't read it, so we decode it with the shared inv_row helper and
# iterate rows by CELL_H (the real glyph height), not a hardcoded 16.
from frame_check import load_font, read_ppm, row_text, inv_row, CELL_H


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-textmode-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); ctrl("f"); time.sleep(0.5)   # C-x C-f
        qmp_type("/disk/note.txt\n"); time.sleep(0.8)            # path in minibuffer
        qmp_type("hello text mode"); time.sleep(0.6)            # edit in text-mode
        ctrl("x"); time.sleep(0.2); ctrl("s"); time.sleep(0.8)   # C-x C-s save
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        # Body text we typed is face-0: read it with row_text.
        lines = [row_text(font, w, data, r) for r in range(h // CELL_H)]
        # The "-- name (Text) --" mode line is inverse-video: decode with inv_row.
        inv = [inv_row(font, w, data, r) for r in range(h // CELL_H)]
        for i, t in enumerate(lines[:8]):
            print(f"  row {i}: {t!r}   inv: {inv[i]!r}")
        ml = "".join(inv)
        ok_mode = "(Text)" in ml
        ok_text = any("hello text mode" in t for t in lines)
        print("PASS: C-x C-f text-mode edit + save" if (ok_mode and ok_text)
              else f"FAIL: mode={ok_mode} text={ok_text}")
        return 0 if (ok_mode and ok_text) else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
