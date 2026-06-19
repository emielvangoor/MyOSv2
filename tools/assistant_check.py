#!/usr/bin/env python3
"""assistant_check.py -- Phase 32.1 (assistant proof-of-life).

Boots `lisp -serve`, loads the assistant Lisp modules off /lib, and asserts
their behaviour over the TCP REPL. The networking tasks add a mock Anthropic
SSE endpoint (tools/mock_anthropic.py) reachable from the guest at 10.0.2.2.

Run from the repo root:  python3 tools/assistant_check.py
"""

import sys
sys.path.insert(0, "tools")
from lm_harness import boot_to_serve, connect_repl, repl_roundtrip


def check(sock, form, want, label):
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
        ok &= check(s, '(load "/lib/json.l")', "", "load json.l")

        # json-escape: a newline becomes the two chars backslash-n.
        ok &= check(s, '(string-length (json-escape (string-from-char 10)))',
                    "2", "json-escape newline -> 2 chars")
        # a double-quote becomes backslash + quote; first char is backslash (92).
        ok &= check(s, '(string-ref (json-escape (string-from-char 34)) 0)',
                    "92", "json-escape quote starts with backslash")
        # a plain ASCII char is unchanged.
        ok &= check(s, '(json-escape "abc")', '"abc"', "json-escape passthrough")

        print("ALL PASS" if ok else "SOME FAILED")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    raise SystemExit(main())
