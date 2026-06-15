#!/usr/bin/env python3
"""
stdin_check.py -- end-to-end check for in-buffer interactive input (Phase 27.1).

The gap this closes: until now a program run from the frame inherited the
frame's fd 0 -- the dead serial console -- so anything that READ stdin (wc,
a prompt) blocked forever on a keyboard nobody was typing on. After 27.1 the
frame feeds its OWN keyboard into the child's stdin, echoing what you type
into the buffer, with C-d for EOF and C-c still killing the job.

We test it with /bin/wc, which counts the bytes on stdin and prints the total:
type "helloworld" + RET (= 11 bytes) into a running wc, send C-d, and the
buffer must show both the echoed input AND wc's answer, "11". A pre-27.1 frame
shows neither (wc blocks on the serial console; typed keys are dropped).

Run from the repo root:  python3 tools/stdin_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text


def qmp_ctrl(letter):
    """Press Ctrl-<letter> as one chord on the virtio keyboard."""
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-stdin-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        qmp_ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)  # C-x r: REPL in this window

        # Start a program that READS stdin and blocks until EOF.
        qmp_type('(run "wc")\n')
        time.sleep(1.0)
        # Type into the running child: 10 chars + RET = 11 bytes on its stdin.
        qmp_type("helloworld\n")
        time.sleep(0.8)
        qmp_ctrl("d")                      # C-d: close stdin -> wc sees EOF
        time.sleep(1.5)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(14)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}")

        ok = True
        if not any(t == "helloworld" for t in lines):
            print("FAIL: typed input did not echo into the buffer"); ok = False
        if not any(t == "11" for t in lines):
            print("FAIL: wc did not receive 11 bytes on stdin (no EOF/no feed)"); ok = False
        if ok:
            print("PASS: 27.1 in-buffer interactive input verified")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
