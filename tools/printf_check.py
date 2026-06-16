#!/usr/bin/env python3
"""
printf_check.py -- TCC compiles a #include <stdio.h> / printf program ON the
machine, against the REAL musl libc baked onto the ext2 /disk (Phase 3).

This is the Phase-3 ext2/sysroot milestone: /disk now carries a musl sysroot at
/disk/usr/{include,lib}, so the on-device /bin/tcc can compile hosted C. The
`cc` Lisp helper reconstructs the static crt link line against that sysroot:

  (cc "/hello.c" "/hello")  -> tcc -nostdlib -static -Ttext=.. -I/disk/usr/include
                               crt1 crti /hello.c -L/disk/usr/lib -lc libtcc1.a
                               crtn -o /hello   (exit 0)
  (run-file "/hello")       -> exec it; printf runs through musl

The default /hello.c (src/initrd.c) is:
    #include <stdio.h>
    int main(void){ printf("hello from tcc on myosv2: x=%d s=%s\n", 42, "ok"); }

So success = the serial shows "x=42 s=ok". Reading ~100 headers from ext2 over
virtio-blk is slow; give tcc a generous timeout.

Run from the repo root:  python3 -u tools/printf_check.py
"""

import sys

sys.path.insert(0, "tools")
from lm_harness import Qemu


def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no boot"); return 1
        # Compile the default printf hello against the on-disk musl sysroot.
        # tcc reading the headers off ext2 is slow -- be patient.
        q.send_line('(cc "/hello.c" "/hello")')
        if not q.expect(b"0", 40):                 # tcc exit status 0 = compiled
            print("FAIL: tcc did not report success (musl link)"); return 1
        q.send_line('(run-file "/hello")')
        if q.expect(b"hello from tcc on myosv2: x=42 s=ok", 15):
            print("PASS: tcc compiled #include <stdio.h> printf against musl on /disk, and it ran")
            return 0
        print("FAIL: the tcc+musl binary did not print the expected printf output")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
