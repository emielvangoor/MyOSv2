#!/usr/bin/env python3
"""
musl_check.py -- a real static musl binary runs on MyOSv2 (Phase 28, B-SP2).

/bin/mhello is built with aarch64-linux-musl-gcc -static (linked into the clean
user VA range). Booting and running it exercises the whole migrated Linux ABI:
the auxv initial stack, openat/read/write/exit_group, plus musl's startup
syscalls (set_tid_address, ioctl->ENOTTY, writev). Success = its printf output
appears on the serial console.

Run from the repo root:  python3 tools/musl_check.py
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
        q.send_line('(run "mhello")')
        if q.expect(b"hello from musl on MyOSv2!", 10):
            print("PASS: B-SP2 -- a static musl binary runs on MyOSv2")
            return 0
        print("FAIL: musl hello output not seen on serial")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
