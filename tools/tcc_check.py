#!/usr/bin/env python3
"""
tcc_check.py -- a C compiler runs ON MyOSv2 and compiles a hello world,
FREESTANDING (no libc), via the `cc-bare` Lisp helper.

The Phase-28 milestone: TCC (a static aarch64-linux-musl build, /bin/tcc)
compiles + links /hellobare.c into a runnable static ELF, on the machine,
linked only against /lib/mycrt.o (a _start + write-syscall stub, no libc), and
that binary runs and prints. (The libc/printf path is now `cc`; see
printf_check.py. This check guards the no-sysroot path stays alive.)

  (cc-bare "/hellobare.c" "/hello") -> tcc -static -nostdlib -Ttext=.. \
                                       /hellobare.c /lib/mycrt.o -o /hello (0)
  (run-file "/hello")               -> exec it; it prints the message

/hellobare.c + /lib/mycrt.o + /bin/tcc are in the initrd.

Run from the repo root:  python3 tools/tcc_check.py
"""

import sys
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu


def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no boot"); return 1
        q.send_line('(cc-bare "/hellobare.c" "/hello")')
        if not q.expect(b"0", 20):                 # tcc exit status 0 = compiled
            print("FAIL: tcc did not report success"); return 1
        q.send_line('(run-file "/hello")')
        if q.expect(b"hello from tcc on myosv2", 10):
            print("PASS: TCC compiled + linked a hello world on MyOSv2, and it ran")
            return 0
        print("FAIL: the tcc-built binary did not print")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
