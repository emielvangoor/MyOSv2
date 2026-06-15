#!/usr/bin/env python3
"""
findfile_check.py -- editable file buffers in the frame (Phase 27.3).

The Emacs-machine step: open a file into a buffer, edit it, save it back to
disk. This drives the whole loop from the keyboard:

  (find-file "/n.txt")   -- opens the (new) file in a split window, selected
  type "hello edit" RET  -- the buffer is now EDITABLE (RET inserts a newline,
                            it is not the REPL), so this is real text entry
  C-x C-s                -- save-buffer writes it to disk
  C-x o                  -- back to the REPL
  (cat "/n.txt")         -- read it back; the saved text must stream in

If the file persisted, the REPL shows the cat output "hello edit" right under
the command. A frame without 27.3 can't even start (find-file is unbound).

Run from the repo root:  python3 tools/findfile_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text


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

        qmp_type('(find-file "/n.txt")\n'); time.sleep(1.0)
        qmp_ctrl("x"); time.sleep(0.2); qmp_type("o"); time.sleep(0.6)   # into file
        qmp_type("hello edit\n"); time.sleep(0.6)   # edit the file buffer
        qmp_ctrl("x"); time.sleep(0.2); qmp_ctrl("s"); time.sleep(0.8)   # save
        qmp_ctrl("x"); time.sleep(0.2); qmp_type("o"); time.sleep(0.6)   # to REPL
        qmp_type('(cat "/n.txt")\n'); time.sleep(1.0)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(16)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}")

        ok = False
        for i, t in enumerate(lines):
            if t.startswith('lisp> (cat "/n.txt")') and i + 1 < len(lines) \
               and lines[i + 1] == "hello edit":
                ok = True
        if ok:
            print("PASS: 27.3 editable file buffers verified (edit + save persisted)")
            return 0
        print("FAIL: saved text did not read back from disk")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
