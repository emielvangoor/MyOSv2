#!/usr/bin/env python3
"""
persist_check.py -- the headline behaviour of the ext2-root work: a file written
on one boot is still there after a reboot of the SAME disk.

The whole point of replacing the ephemeral SFS ramdisk with a genuine ext2
block-device root is persistence: programs can write files that survive power
off/on cycles.  This check is the only test that PROVES that claim end-to-end:

  Boot 1 (fresh image, Qemu(fresh=True)):
      Write a sentinel string to /persist-test.txt via the serial REPL.
      Tear down QEMU (the disk image is now at rest on the host filesystem).

  Boot 2 (REUSE the same scratch image, Qemu(fresh=False)):
      Boot the same disk image.  cat /persist-test.txt.
      The sentinel MUST appear in the output -- it proves the ext2 driver
      flushed the write to the block device and that a cold reboot can read
      it back.  If fresh=False had been fresh=True (a new pristine copy) the
      file would not exist and the test would catch the regression.

Why this works (the SCRATCH_DISK contract):
  SCRATCH_DISK is a module-level constant in lm_harness: it is computed ONCE
  from os.getpid() and never changes within a Python process.  Both Qemu()
  calls below therefore map to exactly the same host-side file.  Boot 1
  (fresh=True) copies the built image there and boots it; after kill() the
  file contains the modified ext2 image.  Boot 2 (fresh=False) skips the
  copy and boots that same modified file, so writes from Boot 1 are visible.

Run from the repo root:
    python3 -u tools/persist_check.py
Expected output: "PERSIST OK" and exit 0.

NOTE: this check is NOT part of `make test` (the KTEST suite).  It boots QEMU
twice and takes a few minutes.  Run it manually to validate ext2 persistence.
"""

import sys

sys.path.insert(0, "tools")
from lm_harness import Qemu

# A unique sentinel string.  It is long enough to be unmistakable and
# contains no Lisp-special characters, so it round-trips through the REPL
# unchanged.
SENTINEL = "PERSISTED-OK-7351"


def main() -> int:
    # ------------------------------------------------------------------
    # Boot 1: start from a pristine copy of the built ext2 image, write
    # the sentinel file, then shut down cleanly.
    # ------------------------------------------------------------------
    print("=== Boot 1: writing sentinel to /persist-test.txt ===")
    q = Qemu(fresh=True)     # copies build/disk.img -> SCRATCH_DISK
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: boot 1 -- REPL prompt never appeared"); return 1

        # Write the sentinel.  We use the raw fd primitives so the write goes
        # through the VirtIO block driver and is visible as a genuine file.
        # (creat path) -> fd  (creates or truncates the file)
        # (fd-write fd str) -> bytes-written
        # (close fd) -> nil  (flushes + releases the fd; ext2 must persist it)
        form = (
            '(let ((fd (creat "/persist-test.txt")))'
            ' (fd-write fd "%s") (close fd))' % SENTINEL
        )
        q.send_line(form)
        # Wait for the REPL to return a result and re-print the prompt.
        # That guarantees the expression has fully evaluated (i.e. the fd is
        # closed and the block write has been submitted to the driver).
        if not q.expect(b"lisp> ", 15):
            print("FAIL: boot 1 -- REPL did not return to prompt after write"); return 1

        print("  sentinel written; shutting down Boot 1")
    finally:
        q.kill()   # QEMU exits; the scratch image is now at rest on disk

    # ------------------------------------------------------------------
    # Boot 2: reuse the SAME scratch image (fresh=False -- no re-copy).
    # The sentinel file must still be readable, proving the ext2 root is
    # genuinely persistent across a reboot.
    # ------------------------------------------------------------------
    print("=== Boot 2: reading /persist-test.txt after reboot ===")
    q = Qemu(fresh=False)    # boots the same SCRATCH_DISK modified by Boot 1
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: boot 2 -- REPL prompt never appeared"); return 1

        # (cat path) -> prints the file contents to the serial console,
        # returns t.  The sentinel must appear in the output stream.
        q.send_line('(cat "/persist-test.txt")')
        if not q.expect(SENTINEL.encode(), 15):
            print(
                "FAIL: sentinel not found after reboot -- "
                "the ext2 root is NOT persistent (or the write did not flush)"
            )
            return 1

        print("  sentinel confirmed in Boot 2 output")
    finally:
        q.kill()

    print("PERSIST OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
