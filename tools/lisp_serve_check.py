#!/usr/bin/env python3
"""
lisp_serve_check.py -- end-to-end check for Phase 24.1b (`lisp -serve`).

Boots the OS under QEMU, starts the Lisp TCP REPL from the shell, then talks to
it from the host over the forwarded port. This is the "boot and observe"
integration test the KTEST suite can't cover (it is inherently userland +
over-the-wire). The boot/serial/REPL plumbing lives in lm_harness.py.

Checks performed:
  1. boot reaches the shell, `lisp -serve` reports it is listening
  2. host connects to localhost:7777, evals (+ 1 2) -> 3
  3. an error comes back over the SOCKET (lm_error -> lm_cur_out),
     not invisibly onto the guest's serial console
  4. a defun made on one connection survives into the next one
     (the image persists across connections -- the whole point)

Exit code 0 = all green. Run from the repo root:  python3 tools/lisp_serve_check.py
"""

import sys
import time

sys.path.insert(0, "tools")
from lm_harness import boot_to_serve, connect_repl, repl_roundtrip


def main() -> int:
    q = boot_to_serve()
    print("ok: shell prompt + server reports listening")
    try:
        # --- connection 1: eval + defun + remote error ---
        s1 = connect_repl()
        out = repl_roundtrip(s1, "(+ 1 2)")
        if "3" not in out:
            print(f"FAIL: (+ 1 2) over TCP, got: {out!r}"); return 1
        print("ok: (+ 1 2) -> 3 over TCP")

        out = repl_roundtrip(s1, "(defun twice (x) (* 2 x))")
        if "twice" not in out:
            print(f"FAIL: defun reply, got: {out!r}"); return 1

        out = repl_roundtrip(s1, "(nosuchfunction 1)")
        if "ERROR" not in out:
            print(f"FAIL: error not reported to the socket, got: {out!r}"); return 1
        s1.close()
        print("ok: defined twice, saw remote ERROR, disconnected")

        # --- connection 2: the image must have survived ---
        time.sleep(0.5)
        s2 = connect_repl()
        out = repl_roundtrip(s2, "(twice 21)")
        if "42" not in out:
            print(f"FAIL: image did not persist across connections, got: {out!r}")
            return 1
        s2.close()
        print("ok: image persisted across reconnect ((twice 21) -> 42)")

        print("PASS: 24.1b TCP REPL verified")
        return 0
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
