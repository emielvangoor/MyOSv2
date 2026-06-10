#!/usr/bin/env python3
"""
input_check.py -- end-to-end check for Phase 25.1 (virtio-input).

Boots the OS, starts /bin/evtest from the Lisp REPL, then injects keyboard and
tablet events through QEMU's QMP interface -- the same path a human's keys and
mouse take into the graphical window -- and asserts evtest prints them.

Run from the repo root:  python3 tools/input_check.py
"""

import sys
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp_key, qmp_tablet


def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot to Lisp prompt"); return 1
        q.send_line('(run "evtest")')
        if not q.expect(b"waiting for events", 10):
            print("FAIL: evtest did not start"); return 1
        time.sleep(0.3)

        # Press + release 'a'. EV_KEY=1, KEY_A=30: "EV 1 30 1" then "EV 1 30 0".
        qmp_key("a")
        if not (q.expect(b"EV 1 30 1", 5) and q.expect(b"EV 1 30 0", 5)):
            print("FAIL: key events did not arrive"); return 1
        print("ok: keyboard events arrive")

        # Move the tablet. EV_ABS=3, ABS_X=0 / ABS_Y=1.
        qmp_tablet(16000, 8000)
        if not q.expect(b"EV 3 0 ", 5):
            print("FAIL: tablet events did not arrive"); return 1
        print("ok: tablet events arrive")

        print("PASS: 25.1 virtio-input verified")
        return 0
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
