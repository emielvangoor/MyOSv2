#!/usr/bin/env python3
"""
gfx_check.py -- end-to-end check for userland graphics in the frame.

A program puts pixels on screen by drawing into a SURFACE buffer's shared-memory
canvas, which the redisplay engine blits into its window (drawing the raw
framebuffer no longer works once init autostarts the frame -- the frame owns the
screen and repaints over it). This drives the frame's `(gfxtest)` command, which
runs /bin/gfxtest (red/green/blue vertical thirds + a white square) into a
surface buffer, screendumps the scanout, and asserts the three colors are
present -- proving the chain: userland write -> shm canvas -> rd_redisplay blit
-> virtio-gpu TRANSFER/FLUSH -> the display.

Run from the repo root:  python3 tools/gfx_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import read_ppm


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def count_pure(body, r0, g0, b0, tol=60):
    """Count near-pure (r0,g0,b0) pixels across the whole scanout."""
    n = 0
    for i in range(0, len(body) - 3, 3):
        if abs(body[i] - r0) < tol and abs(body[i + 1] - g0) < tol and abs(body[i + 2] - b0) < tol:
            n += 1
    return n


def main() -> int:
    dump = os.path.join(tempfile.gettempdir(), "myosv2-gfx-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)   # C-x r: a REPL buffer
        qmp_type('(gfxtest)\n'); time.sleep(2.5)                      # surface pattern
        qmp_screendump(dump); time.sleep(0.3)

        w, h, body = read_ppm(dump)
        red = count_pure(body, 255, 0, 0)
        grn = count_pure(body, 0, 255, 0)
        blu = count_pure(body, 0, 0, 255)
        print("pure red=%d green=%d blue=%d" % (red, grn, blu))
        if red > 1000 and grn > 1000 and blu > 1000:
            print("PASS: gfxtest surface pattern (R/G/B thirds) renders in the frame")
            return 0
        print("FAIL: gfxtest pattern not rendered in the frame")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
