#!/usr/bin/env python3
"""
busybox_check.py -- a real static-musl busybox runs on MyOSv2 (Phase 28, B-SP4).

/bin/busybox is a 1.25 MB static aarch64 busybox (built from source with
CONFIG_STATIC and -Wl,-Ttext-segment=0x8000000000; the binary is committed at
user/musl/busybox.bin). Running its `echo` applet proves the migrated Linux ABI
carries a real, complex multi-call Unix binary -- argv handling, the larger ELF
image, and the startup syscalls.

Run from the repo root:  python3 tools/busybox_check.py
"""

import sys
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu


def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "busybox" "echo" "BBX-RUNS-ON-MYOSV2")')
        if q.expect(b"BBX-RUNS-ON-MYOSV2", 10):
            print("PASS: B-SP4 -- static-musl busybox runs on MyOSv2")
            return 0
        print("FAIL: busybox echo output not seen")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
