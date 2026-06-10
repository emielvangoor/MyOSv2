#!/usr/bin/env python3
"""
lisp_shell_check.py -- end-to-end check for Phase 24.3 (the shell in Lisp).

system.l is the Eshell-hybrid shell: (run "cmd" args...) fork/execs an ELF from
/bin and waits; (| a b) connects two stages with a pipe; (ls)/(cat) are
coreutils written in Lisp on the syscall primitives. All of it is userland
behavior against the real kernel, so the test boots QEMU and observes.

Phase 1 drives the SERIAL REPL (paced char-by-char -- see lm_harness.py),
because pipelines redirect fd 1 and on the console fd 1 is where the REPL's
own output goes, so in-image stages compose there. Phase 2 re-checks the
Lisp-written coreutils over the network REPL.

Run from the repo root:  python3 tools/lisp_shell_check.py
"""

import sys
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, boot_to_serve, connect_repl, repl_roundtrip


def serial_step(q: Qemu, form: str, want: bytes, label: str) -> bool:
    """Type `form` at the serial REPL; `want` must appear before the next prompt."""
    q.send_line(form)
    if not q.expect(want, 15):
        print(f"FAIL: {label}\n  form: {form}\n  want: {want!r}\n  tail: {q.buf[-200:]!r}")
        return False
    print(f"ok: {label}")
    return True


def main() -> int:
    ok = True

    # ---- phase 1: the serial REPL, where fd 1 is the console ----
    q = Qemu()
    try:
        if not q.expect(b"$ ", 30):
            print("FAIL: never saw the shell prompt"); return 1
        q.send_line("lisp")
        if not q.expect(b"system.l loaded", 15):
            print("FAIL: /bin/lisp did not load system.l"); return 1
        if not q.expect(b"lisp> ", 5):
            print("FAIL: no REPL prompt after load"); return 1
        print("ok: lisp boots and loads system.l")

        # (run ...): fork+exec /bin/hello, wait; prints on the console, evals
        # to the child's exit status.
        ok &= serial_step(q, '(run "hello")', b"Hello from /bin/hello", "(run \"hello\") runs the ELF")

        # An external|external pipeline: hello's 22 bytes flow into wc.
        ok &= serial_step(q, '(| (run "hello") (run "wc"))', b"22",
                          "(| (run hello) (run wc)) -> 22")

        # An IN-IMAGE stage feeding an external program: princ writes to fd 1,
        # which the pipeline has dup2'd into the pipe. The Eshell promise.
        ok &= serial_step(q, '(| (princ "abcde") (run "wc"))', b"5",
                          "(| (princ ...) (run wc)) -> 5")

        # Lisp coreutils.
        ok &= serial_step(q, '(cat "/motd")', b"Welcome to MyOSv2.", "(cat \"/motd\")")
        ok &= serial_step(q, '(ls "/bin")', b"lisp", "(ls \"/bin\") lists /bin")

        # Leave the REPL; the C shell must still be there (we are not init yet).
        q.send_line("(exit 0)")
        if not q.expect(b"$ ", 10):
            print("FAIL: (exit 0) did not return to the C shell"); ok = False
        else:
            print("ok: (exit 0) returns to the C shell")
    finally:
        q.kill()
    if not ok:
        return 1

    # ---- phase 2: the same shell vocabulary over the network REPL ----
    q = boot_to_serve()
    try:
        s = connect_repl()

        out = repl_roundtrip(s, '(cat "/motd")')
        if "Welcome to MyOSv2." not in out:
            print(f"FAIL: (cat) over TCP, got: {out!r}"); ok = False
        else:
            print("ok: (cat \"/motd\") over TCP")

        out = repl_roundtrip(s, '(ls "/bin")')
        if "lisp" not in out or "wc" not in out:
            print(f"FAIL: (ls \"/bin\") over TCP, got: {out!r}"); ok = False
        else:
            print("ok: (ls \"/bin\") over TCP")

        # (run ...) evals to the exit status over the socket; the program's own
        # output lands on the guest console (fd 1 is not the socket).
        out = repl_roundtrip(s, '(run "hello")')
        if "0" not in out:
            print(f"FAIL: (run \"hello\") status over TCP, got: {out!r}"); ok = False
        elif not q.expect(b"Hello from /bin/hello", 5):
            print("FAIL: hello's output did not reach the console"); ok = False
        else:
            print("ok: (run \"hello\") over TCP -> status 0, output on console")

        s.close()
    finally:
        q.kill()

    if ok:
        print("PASS: 24.3 Lisp shell verified")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
