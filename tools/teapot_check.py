#!/usr/bin/env python3
"""
teapot_check.py -- end-to-end check for Phase 26.2 (TinyGL teapot).

Three claims, verified by screendump:
  1. The FULL teapot renders: the GLUT data stores one quadrant (plus half a
     handle/spout); the renderer must mirror it out. A quarter-pot shows only
     a few hundred lit pixels -- the full brass body shows thousands.
  2. It spins while the machine is idle (the animate heartbeat repaints).
  3. It KEEPS spinning while a slow-printing child streams into the REPL:
     ping prints one line a second, and stream-thunk must repaint on its
     50ms poll ticks, not only when a chunk arrives (the 1-fps bug).

Run from the repo root:  python3 tools/teapot_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import read_ppm, CELL_H, GFX_H


def qmp_ctrl(letter):
    """Press Ctrl-<letter> as one chord on the virtio keyboard."""
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})

# (teapot) splits below: the surface lands in the bottom window, whose top
# edge is where the top window's 50% share of the rows ends (echo line off).
_ROWS = GFX_H // CELL_H
SURF_Y = ((_ROWS - 1) * 50 // 100) * CELL_H
SURF_W, SURF_H = 480, 300          # sample comfortably inside the canvas


def region(data, w, x0, y0, x1, y1):
    return [data[3 * (y * w + x):3 * (y * w + x) + 3]
            for y in range(y0, y1) for x in range(x0, x1, 2)]


def lit_count(data, w):
    """Brass-ish pixels in the surface area: clearly bright, warm-toned."""
    n = 0
    for px in region(data, w, 0, SURF_Y, SURF_W, SURF_Y + SURF_H):
        r, g, b = px
        if r > 110 and r > g > b:
            n += 1
    return n


def surface_diff(a, b, w):
    ra = region(a, w, 0, SURF_Y, SURF_W, SURF_Y + SURF_H)
    rb = region(b, w, 0, SURF_Y, SURF_W, SURF_Y + SURF_H)
    return sum(1 for pa, pb in zip(ra, rb) if pa != pb)


def repl_diff(a, b, w):
    """Changed pixels in the TOP window -- where ping's lines stream."""
    ra = region(a, w, 0, 0, 800, SURF_Y - CELL_H)
    rb = region(b, w, 0, 0, 800, SURF_Y - CELL_H)
    return sum(1 for pa, pb in zip(ra, rb) if pa != pb)


def dump(tag):
    path = os.path.join(tempfile.gettempdir(), f"myosv2-teapot-{tag}.ppm")
    qmp_screendump(path)
    time.sleep(0.15)               # let QEMU finish writing the file
    return read_ppm(path)


def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        qmp_ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)  # C-x r: REPL in this window

        qmp_type("(teapot)\n")
        if not q.expect(b"teapot: spinning", 15):
            print("FAIL: /bin/teapot did not start"); return 1
        time.sleep(2.0)

        # 1+2: full pot, spinning at idle.
        w, h, d0 = dump("a")
        time.sleep(0.5)
        w, h, d1 = dump("b")
        lit = lit_count(d1, w)
        moved = surface_diff(d0, d1, w)
        print(f"lit teapot pixels: {lit}, changed while idle: {moved}")
        if lit < 1500:
            print("FAIL: too few lit pixels -- quarter pot or dark render")
            return 1
        if moved < 200:
            print("FAIL: surface static -- the animate heartbeat is dead")
            return 1

        # 3: start a 1-line-a-second stream child, then demand the surface
        # changes across BOTH of two sub-second gaps (at most one gap can
        # contain a ping line, so this only passes if redisplay runs on the
        # quiet poll ticks too).
        qmp_type('(run "ping" "10.0.2.2")\n')
        time.sleep(2.0)
        w, h, c0 = dump("c")
        time.sleep(0.35)
        w, h, c1 = dump("d")
        time.sleep(0.35)
        w, h, c2 = dump("e")
        m1, m2 = surface_diff(c0, c1, w), surface_diff(c1, c2, w)
        print(f"changed during ping stream: {m1}, {m2} (two 0.35s gaps)")
        if min(m1, m2) < 200:
            print("FAIL: teapot frozen between ping lines (1-fps regression)")
            return 1

        # ...and ping really was streaming into the top window meanwhile.
        time.sleep(1.5)
        w, h, c3 = dump("f")
        if repl_diff(c0, c3, w) < 50:
            print("FAIL: no ping output appeared in the REPL window")
            return 1
        print("ok: ping lines streamed into the REPL while the pot spun")

        # 4: C-c must interrupt the WHOLE job. The stream child is only a
        # Lisp wrapper; /bin/ping is its child -- the group kill (Phase 26.3
        # process groups) is what reaches it. After C-c: no more ping lines
        # (REPL window static), teapot still spinning.
        qmp_ctrl("c")
        time.sleep(2.5)                # let the job die + the prompt repaint
        w, h, e0 = dump("g")
        time.sleep(1.3)                # > one ping interval
        w, h, e1 = dump("h")
        repl_after = repl_diff(e0, e1, w)
        pot_after = surface_diff(e0, e1, w)
        print(f"after C-c: repl changed {repl_after}, teapot changed {pot_after}")
        if repl_after > 50:
            print("FAIL: ping still printing after C-c -- group kill missed it")
            return 1
        if pot_after < 200:
            print("FAIL: teapot stopped after C-c")
            return 1

        print("PASS: 26.2 teapot -- full pot, spins idle AND during streams, "
              "C-c kills the job")
        return 0
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
