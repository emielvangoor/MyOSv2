#!/usr/bin/env python3
"""
lisp_sys_check.py -- end-to-end check for Phase 24.2 (system-call primitives).

Boots the OS, starts `lisp -serve`, and exercises each Lisp primitive that
wraps a syscall, over TCP, against the real kernel. These primitives are
user-build-only (the kernel KTEST build has no syscalls to wrap), so this
boot-and-observe script is their test.

Run from the repo root:  python3 tools/lisp_sys_check.py
"""

import sys

sys.path.insert(0, "tools")
from lm_harness import boot_to_serve, connect_repl, repl_roundtrip


def check(sock, form, want, label):
    """Eval `form`; the reply must contain `want` and must not be an ERROR."""
    out = repl_roundtrip(sock, form)
    if want not in out or "ERROR" in out:
        print(f"FAIL: {label}\n  form: {form}\n  want: {want!r}\n  got:  {out!r}")
        return False
    print(f"ok: {label}")
    return True


def main() -> int:
    q = boot_to_serve()
    try:
        s = connect_repl()
        ok = True

        # (getpid) -> a positive pid
        out = repl_roundtrip(s, "(< 0 (getpid))")
        if "t" not in out:
            print(f"FAIL: (getpid), got: {out!r}"); ok = False
        else:
            print("ok: (getpid) is positive")

        # Files: open /motd (placed by the initrd), read it, close it.
        ok &= check(s, '(setq fd (open "/motd"))', "", "(open \"/motd\")")
        ok &= check(s, "(< fd 0)", "nil", "open returned a valid fd")
        ok &= check(s, "(fd-read fd 7)", '"Welcome"', "(fd-read fd 7)")
        ok &= check(s, "(close fd)", "nil", "(close fd)")

        # Pipes: write into one end, read from the other -- same process.
        ok &= check(s, "(setq p (pipe))", "(", "(pipe) returns (rfd . wfd)")
        ok &= check(s, '(fd-write (cdr p) "hi")', "2", "fd-write into the pipe")
        ok &= check(s, "(fd-read (car p) 10)", '"hi"', "fd-read out of the pipe")

        # fork + exit + wait: the child exits 7; (wait) -> (pid . 7).
        ok &= check(s, "(if (= (fork) 0) (exit 7) (cdr (wait)))",
                    "7", "fork/exit/wait roundtrip")

        # fork + exec + wait: run /bin/true, reap status 0.
        ok &= check(s,
                    '(if (= (fork) 0) (exec "/bin/true" (list "true")) (cdr (wait)))',
                    "0", "fork/exec(/bin/true)/wait")

        # exec of a missing path must fail (return), not vanish.
        ok &= check(s,
                    '(if (= (fork) 0) (progn (exec "/bin/nosuch" (list "x")) (exit 127)) (cdr (wait)))',
                    "127", "exec of a missing program returns")

        # Sockets exist as primitives (full TCP is already proven by this REPL).
        ok &= check(s, "(setq sk (socket 'stream))", "", "(socket 'stream)")
        ok &= check(s, "(< sk 0)", "nil", "socket returned a valid fd")
        ok &= check(s, "(close sk)", "nil", "(close sk)")

        s.close()
        if ok:
            print("PASS: 24.2 system primitives verified")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
