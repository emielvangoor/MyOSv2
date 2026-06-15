#!/usr/bin/env python3
"""
lineedit_check.py -- check for cooked line editing in the frame (Phase 27.2).

27.1 forwarded every typed byte to the child raw, so backspace went down the
pipe as a literal char-8 and the program saw "help\x08lo". A real terminal is
in *canonical* (cooked) mode: it buffers the line, lets you edit it, and only
delivers it on RET. This test types "help", a BACKSPACE, then "lo" -- the child
must receive "hello" (6 bytes with the newline), not "help" with an embedded
backspace. We use /bin/wc (counts the bytes on stdin): a clean "6" proves the
edit happened locally, before the bytes were sent.

Run from the repo root:  python3 tools/lineedit_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_key, qmp_type, qmp_screendump
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
    dump = os.path.join(tempfile.gettempdir(), "myosv2-lineedit-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        qmp_ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)  # C-x r: REPL in this window

        qmp_type('(run "wc")\n')
        time.sleep(1.0)
        qmp_type("help")                   # 4 chars
        time.sleep(0.2)
        qmp_key("backspace")               # edit: drop the 'p' -> "hel"
        time.sleep(0.3)
        qmp_type("lo")                     # -> "hello"
        time.sleep(0.2)
        qmp_type("\n")                     # RET: deliver the cooked line
        time.sleep(0.5)
        qmp_ctrl("d")                      # EOF
        time.sleep(1.5)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(14)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}")

        ok = True
        if not any(t == "hello" for t in lines):
            print("FAIL: edited line is not 'hello' (backspace not cooked)"); ok = False
        if not any(t == "6" for t in lines):
            print("FAIL: wc != 6 bytes -- raw backspace/extra bytes reached stdin"); ok = False
        if ok:
            print("PASS: 27.2 cooked line editing verified")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
