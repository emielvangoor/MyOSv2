#!/usr/bin/env python3
"""
modes_check.py -- unit tests for the pure major-mode model (modes.l), driven
over the plain serial REPL (which loads bootstrap -> system -> modes, but NOT
frame). Each assertion sends a form and checks the printed value.
"""
import sys
sys.path.insert(0, "tools")
from lm_harness import Qemu


def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1

        def check(form, want):
            q.send_line(form)
            ok = q.expect(want.encode(), 5)
            print(("ok  " if ok else "FAIL") + f" {form!r} -> {want!r}")
            return ok

        # Fixtures: a parent mode 'p and a child 'c that inherits its handlers.
        q.send_line("(register-mode 'p (list nil nil 'pins 'pret nil \"P doc\" \"P\"))")
        q.send_line("(register-mode 'c (list 'p nil nil nil nil nil \"C\"))")

        all_ok = True
        all_ok &= check("(mode-parent 'c)", "p")          # derived parent
        all_ok &= check("(mode-return-fn 'c)", "pret")     # inherited handler
        all_ok &= check("(mode-self-insert-fn 'c)", "pins")
        all_ok &= check("(mode-doc 'c)", "P doc")          # inherited doc
        all_ok &= check("(mode-line-name-of 'c)", "C")     # own field wins
        # buffer-local round-trip (handles are arbitrary fixnums here)
        q.send_line("(buffer-local-set 7 'x 42)")
        all_ok &= check("(buffer-local-get 7 'x 0)", "42")
        all_ok &= check("(buffer-local-get 7 'y 99)", "99")  # default
        all_ok &= check("(buffer-local-get 8 'x 0)", "0")    # independent handle
        all_ok &= check("(buffer-mode 8)", "fundamental-mode")  # default mode
        # keymap-chain lookup: a key in the child's keymap is found; absent -> nil
        q.send_line("(register-mode 'k (list nil (list (list 'ctrl 106 'foo \"C-j\")) nil nil nil nil \"K\"))")
        all_ok &= check("(nth 2 (mode-key-lookup 'k '(ctrl 106)))", "foo")
        all_ok &= check("(mode-key-lookup 'k '(ctrl 97))", "nil")

        # register-mode replaces (does not duplicate) an existing entry.
        q.send_line("(register-mode 'p (list nil nil 'pins2 nil nil nil \"P2\"))")
        all_ok &= check("(length (filter (lambda (e) (eq (car e) 'p)) *modes*))", "1")
        all_ok &= check("(mode-self-insert-fn 'p)", "pins2")  # newest wins
        # a key bound in a PARENT's keymap is found when looked up on the child.
        q.send_line("(register-mode 'kc (list 'k nil nil nil nil nil \"KC\"))")
        all_ok &= check("(nth 2 (mode-key-lookup 'kc '(ctrl 106)))", "foo")

        if all_ok:
            print("PASS: mode model verified")
            return 0
        print("FAIL: a model assertion failed")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
