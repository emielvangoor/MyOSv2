#!/usr/bin/env python3
"""
ls_check.py -- busybox `ls` reads a directory via getdents64 (Phase 28).

`ls` is the first applet that needs getdents64 (Linux syscall 61): it opens a
directory fd with openat() and then calls getdents64() to pull packed
`struct linux_dirent64` records. This boots MyOSv2, runs `busybox ls /bin`, and
checks that a known initrd program (`mhello`) shows up in the listing -- proving
the kernel fills and returns dirent records the real binary can parse.

Run from the repo root:  python3 tools/ls_check.py
"""

import sys

sys.path.insert(0, "tools")
from lm_harness import Qemu


def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no boot"); return 1
        q.send_line('(run "busybox" "ls" "/bin")')
        if q.expect(b"mhello", 12):
            print("PASS: busybox ls listed /bin via getdents64 (saw mhello)")
            return 0
        print("FAIL: ls output not seen")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
