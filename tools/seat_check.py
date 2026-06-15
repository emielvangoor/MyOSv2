#!/usr/bin/env python3
"""
seat_check.py -- end-to-end check for Phase 25.5 (seats: multiple Lisp VMs).

Boots VM1's frame, (spawn-vm)s a second complete Lisp machine, then VT-switches
with Ctrl-Alt-F2 / Ctrl-Alt-F1 and proves BY SCREENDUMP that each seat shows
its own independent frame: VM2 evals (* 6 7) -> 42 on its screen; switching
back, VM1 still shows its own history and no 42.

Run from the repo root:  python3 tools/seat_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text


def hotkey_seat(n):
    """Ctrl-Alt-Fn, pressed like a human would."""
    fkey = f"f{n}"
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "alt"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": fkey}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": fkey}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "alt"}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def screen_rows(font, dump, n=8):
    qmp_screendump(dump)
    time.sleep(0.5)
    w, h, data = read_ppm(dump)
    return [row_text(font, w, data, r) for r in range(n)]


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-seat-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: VM1 frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)  # C-x r: REPL in this window

        qmp_type("(spawn-vm)\n")
        time.sleep(4.0)                       # the child loads its libraries

        rows = screen_rows(font, dump)
        if not any(t.startswith("lisp> (spawn-vm)") for t in rows):
            print(f"FAIL: VM1 lost its history: {rows}"); return 1
        print("ok: VM1 active, second VM spawned in the background")

        hotkey_seat(2)
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)  # C-x r: REPL on VM2's own frame
        qmp_type("(* 6 7)\n")
        time.sleep(1.0)
        rows = screen_rows(font, dump)
        if any("spawn-vm" in t for t in rows):
            print(f"FAIL: seat 2 shows VM1's frame: {rows}"); return 1
        if not any(t == "42" for t in rows):
            print(f"FAIL: VM2 did not eval on its own frame: {rows}"); return 1
        print("ok: Ctrl-Alt-F2 -> a fresh, independent Lisp machine (42 on screen)")

        hotkey_seat(1)
        time.sleep(1.0)
        rows = screen_rows(font, dump)
        if not any(t.startswith("lisp> (spawn-vm)") for t in rows):
            print(f"FAIL: VM1's frame did not survive the switch: {rows}"); return 1
        if any(t == "42" for t in rows):
            print(f"FAIL: VM1 shows VM2's content: {rows}"); return 1
        print("ok: Ctrl-Alt-F1 -> VM1 exactly as it was")

        print("PASS: 25.5 seats verified (two live VMs, hotkey switching)")
        return 0
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
