#!/usr/bin/env python3
"""
mx_check.py -- end-to-end check for M-x + describe-function (vertico-style).

Boots the frame, presses M-x, types a fragment, verifies the live-narrowing
candidate list appears in the grown echo area, RETs the selection and checks
the command actually ran (split-below -> a second modeline). Then C-h f
describes a function into the *Help* window and the source text is read off
the screendump.

Run from the repo root:  python3 tools/mx_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, CELL_H


def meta_x():
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "alt"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "x"}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "x"}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "alt"}}}]})


def ctrl(key):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": key}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": key}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def rows_of(font, dump, n_rows):
    qmp_screendump(dump)
    time.sleep(0.5)
    w, h, data = read_ppm(dump)
    total = h // CELL_H
    return [row_text(font, w, data, r, 50) for r in range(total - n_rows, total)], \
           [row_text(font, w, data, r, 50) for r in range(total)]


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-mx-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        # M-x spli -> the candidate list must narrow to the split commands.
        meta_x()
        time.sleep(0.5)
        qmp_type("spli")
        time.sleep(1.5)
        tail, _ = rows_of(font, dump, 6)
        flat = " | ".join(t for t in tail if t)
        if "M-x spli" not in flat or "split-below" not in flat:
            print(f"FAIL: vertico candidates missing: {flat!r}"); return 1
        print(f"ok: M-x narrows live: {flat!r}")

        # C-n to split-right... order depends on filtering; just RET the
        # selection and verify SOME split happened (two modelines on screen).
        qmp_type("\n")
        time.sleep(1.5)
        # (Inverse-video modelines don't glyph-decode, so detect the split by
        # the banner appearing in BOTH windows showing *repl*.)
        _, allrows = rows_of(font, dump, 1)
        banners = [r for r in allrows if r.startswith("MyOSv2 Graphical")]
        if len(banners) < 2:
            print(f"FAIL: M-x split did not run: {allrows!r}"); return 1
        print(f"ok: M-x ran the command (window split visible)")

        # C-h f -> describe `take` -> *Help* shows its parameters and source.
        ctrl("h")
        time.sleep(0.3)
        qmp_type("f")
        time.sleep(0.5)
        qmp_type("take")
        time.sleep(1.5)
        qmp_type("\n")
        time.sleep(1.5)
        _, allrows = rows_of(font, dump, 1)
        flat = " ".join(allrows)
        if "take is a lambda" not in flat or "parameters:" not in flat:
            print(f"FAIL: describe-function output missing: {flat!r}"); return 1
        print("ok: C-h f shows the living source in *Help*")

        print("PASS: M-x + describe-function (vertico) verified")
        return 0
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
