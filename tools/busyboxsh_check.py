#!/usr/bin/env python3
"""
busyboxsh_check.py -- (run "busybox" "sh") is a usable INTERACTIVE shell.

This guards the whole "busybox sh works" feature end to end:
  - the TTY ioctls (TCGETS) make isatty(0) true, so ash prints its "/ #" prompt;
  - getpgid/ppoll keep ash's job-control loop from spinning / flooding the console;
  - the /bin/<applet> -> busybox symlinks (baked by mke2fs -d) plus the kernel's
    symlink-following path resolution make a bare `ls` run the busybox applet,
    which lists the real ext2 root;
  - `exit` returns control to the Lisp REPL.

Boots a scratch copy of the disk image (non-destructive) and drives the serial
console. Run from the repo root:  python3 tools/busyboxsh_check.py
"""
import sys
sys.path.insert(0, "tools")
from lm_harness import Qemu


def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no Lisp REPL prompt at boot")
            return 1
        q.send_line('(run "busybox" "sh")')
        # ash's interactive prompt. If this never appears, isatty/TTY ioctls or
        # the getpgid job-control loop are broken.
        if not q.expect(b"/ #", 15):
            print("FAIL: no busybox sh prompt -- TTY ioctls / getpgid not working")
            return 1
        # A bare `ls` must resolve via the /bin symlink to the busybox applet and
        # list the real ext2 root (entry names appear even amid ANSI color codes).
        q.send_line("ls /")
        if not q.expect(b"usr", 10):
            print("FAIL: `ls` did not resolve to the busybox applet / no listing")
            return 1
        # A builtin, to confirm the shell is really executing commands.
        q.send_line("echo BBSH_RAN_OK")
        if not q.expect(b"BBSH_RAN_OK", 10):
            print("FAIL: `echo` did not run")
            return 1
        q.send_line("exit")
        if not q.expect(b"lisp> ", 10):
            print("FAIL: did not return to the Lisp REPL after exit")
            return 1
        print("BUSYBOX-SH OK")
        return 0
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
