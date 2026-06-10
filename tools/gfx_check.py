#!/usr/bin/env python3
"""
gfx_check.py -- end-to-end check for Phase 25.2 (virtio-gpu framebuffer).

Boots the OS, runs /bin/gfxtest (red/green/blue vertical thirds + a white 8x8
square at (8,8)), then takes a QMP screendump of the scanout and asserts the
pixels -- proving the whole chain: userland write -> mapped framebuffer ->
TRANSFER_TO_HOST_2D + RESOURCE_FLUSH -> the actual display surface.

Run from the repo root:  python3 tools/gfx_check.py
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp_screendump, ppm_pixel


def close(p, want, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(p, want))


def main() -> int:
    dump = os.path.join(tempfile.gettempdir(), "myosv2-gfx-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "gfxtest")')
        if not q.expect(b"pattern drawn", 10):
            print("FAIL: gfxtest did not draw"); return 1
        time.sleep(0.5)

        qmp_screendump(dump)
        time.sleep(0.5)                       # screendump is async-ish; settle

        checks = [
            ((100,  100), (255, 0, 0),   "red third"),
            ((640,  100), (0, 255, 0),   "green third"),
            ((1200, 100), (0, 0, 255),   "blue third"),
            ((12,   12),  (255, 255, 255), "white square"),
        ]
        ok = True
        for (x, y), want, label in checks:
            got = ppm_pixel(dump, x, y)
            if close(got, want):
                print(f"ok: {label} at ({x},{y}) = {got}")
            else:
                print(f"FAIL: {label} at ({x},{y}): want ~{want}, got {got}")
                ok = False
        if ok:
            print("PASS: 25.2 virtio-gpu framebuffer verified")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
