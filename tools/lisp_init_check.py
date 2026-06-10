#!/usr/bin/env python3
"""
lisp_init_check.py -- end-to-end check for Phase 24.4 (init IS the Lisp machine).

The capstone of Phase 24: PID 1 is /bin/lisp. The machine boots INTO the
Lisp REPL -- no C shell in between -- and the C shell survives as an ordinary
command (`(run "sh")`), inverting the old relationship.

Checks performed:
  1. boot lands directly at the Lisp prompt (bootstrap.l + system.l loaded
     by init; no `$ ` shell prompt first)
  2. the shell vocabulary works as init: (run "hello"), a pipeline
  3. (run "sh") starts the C shell; it works; `exit` returns to Lisp
  4. init survives: after all that, the REPL still answers

Run from the repo root:  python3 tools/lisp_init_check.py
"""

import sys

sys.path.insert(0, "tools")
from lm_harness import Qemu


def step(q: Qemu, line: str, want: bytes, label: str) -> bool:
    q.send_line(line)
    if not q.expect(want, 15):
        print(f"FAIL: {label}\n  sent: {line}\n  want: {want!r}\n  tail: {q.buf[-200:]!r}")
        return False
    print(f"ok: {label}")
    return True


def main() -> int:
    q = Qemu()
    try:
        # Boot must land at the Lisp REPL without us typing anything. If the
        # C shell's `$ ` shows up first, init is not the Lisp machine.
        if not q.expect(b"system.l loaded", 30):
            print("FAIL: init did not load system.l at boot"); return 1
        if not q.expect(b"lisp> ", 10):
            print("FAIL: no Lisp prompt from init"); return 1
        if b"$ " in q.buf:
            print("FAIL: saw the C shell prompt before the Lisp REPL"); return 1
        print("ok: machine boots into the Lisp REPL (PID 1)")

        ok = True
        ok &= step(q, "(getpid)", b"1", "init is PID 1")
        ok &= step(q, '(run "hello")', b"Hello from /bin/hello", "(run \"hello\") under init")
        ok &= step(q, '(| (run "hello") (run "wc"))', b"22", "pipeline under init")

        # The inversion: the C shell is now just a command.
        ok &= step(q, '(run "sh")', b"$ ", "(run \"sh\") starts the C shell")
        ok &= step(q, "hello", b"Hello from /bin/hello", "the C shell still works")
        ok &= step(q, "exit", b"lisp> ", "`exit` falls back to the Lisp REPL")

        # And the image is still alive after all of that.
        ok &= step(q, "(+ 40 2)", b"42", "init's REPL still answers")

        if ok:
            print("PASS: 24.4 Lisp init verified")
            return 0
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
