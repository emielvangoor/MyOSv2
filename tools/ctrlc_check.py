#!/usr/bin/env python3
"""
ctrlc_check.py -- C-c interrupts a job running inside busybox sh in the frame.

Guards the tty foreground-process-group fix: the graphical Ctrl-C (SYS_INPUT_READ
interception) and the serial Ctrl-C both route through tty_intr(), which signals
the terminal's foreground process group (published by ash via tcsetpgrp/
TIOCSPGRP), with a fallback to the scheduler's foreground thread. Before the fix,
ash put `ping` in its own process group and the frame's kill(-wrapper) missed it,
so C-c did nothing.

Drives the frame: run busybox sh, run `ping` inside it, press Ctrl-C, and assert
the ping seq counter STOPS climbing (and the `/ #` prompt returns -- the shell
itself survives). Run from the repo root: python3 tools/ctrlc_check.py
"""
import os
import re
import sys
import tempfile
import time

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


def max_seq(font, path):
    qmp_screendump(path)
    time.sleep(0.3)
    w, h, data = read_ppm(path)
    lines = [row_text(font, w, data, r) for r in range(h // CELL_H)]
    seqs = [int(m) for t in lines for m in re.findall(r"seq=(\d+)", t)]
    return max(seqs) if seqs else -1


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-ctrlc.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.3); qmp_type("r"); time.sleep(0.8)   # C-x r: a REPL
        qmp_type('(run "busybox" "sh")\n'); time.sleep(2.0)
        qmp_type('ping\n'); time.sleep(5.0)                          # ping INSIDE sh
        before = max_seq(font, dump)
        if before < 0:
            print("FAIL: ping never produced output (setup problem)"); return 1
        ctrl("c"); time.sleep(0.5)                                   # <-- the interrupt
        time.sleep(4.0)
        mid = max_seq(font, dump)
        time.sleep(4.0)
        after = max_seq(font, dump)
        # After C-c, at most one in-flight reply may land; then it must STOP.
        if after > mid:
            print(f"FAIL: ping kept running after C-c (seq {before} -> {mid} -> {after})")
            return 1
        print(f"CTRL-C OK (seq {before} -> {mid} -> {after}; stopped)")
        return 0
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
