#!/usr/bin/env python3
"""
frametcc_check.py -- compiling + running C IN THE FRAME streams to the buffer.

Regression guard: (run-file ...) must use the pluggable run-prog so a program's
output lands in the current buffer (not the serial). Drives the frame: C-x r to
get a REPL, (cc "/hello.c" "/hello"), then (run-file "/hello"), and OCRs the
buffer for the message the tcc-built binary prints.
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
    dump = os.path.join(tempfile.gettempdir(), "myosv2-frametcc.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)     # C-x r: a REPL
        qmp_type('(cc "/hello.c" "/hello")\n'); time.sleep(5.0)        # compile in-frame
        qmp_type('(run-file "/hello")\n'); time.sleep(2.5)            # run -> streams to buffer
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(h // CELL_H)]
        for i, t in enumerate(lines):
            if t.strip(): print(f"  row {i}: {t!r}")
        ok = any("hello from tcc" in t for t in lines)
        print("PASS: tcc-built program output streamed into the frame buffer" if ok
              else "FAIL: output not in the buffer (still on serial?)")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
