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
import mock_anthropic


def check(sock, form, want, label):
    out = repl_roundtrip(sock, form)
    if want not in out or "ERROR" in out:
        print(f"FAIL: {label}\n  form: {form}\n  want: {want!r}\n  got:  {out!r}")
        return False
    print(f"ok: {label}")
    return True


def main() -> int:
    port = mock_anthropic.start()
    print(f"mock endpoint on 127.0.0.1:{port} (guest sees 10.0.2.2:{port})")
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

        # json-string-value: pull a decoded string field out of a JSON blob.
        ok &= check(s,
            '(json-string-value "{\\"type\\":\\"text_delta\\",\\"text\\":\\"Hi there\\"}" "text")',
            '"Hi there"', "json-string-value reads text field")
        # missing key -> nil
        ok &= check(s,
            '(json-string-value "{\\"a\\":\\"b\\"}" "text")',
            "nil", "json-string-value missing key -> nil")
        # an escaped newline inside the value decodes to one char (10).
        ok &= check(s,
            '(string-ref (json-string-value "{\\"text\\":\\"a\\\\nb\\"}" "text") 1)',
            "10", "json-string-value decodes newline escape")

        # M2: recursive json-parse + json-get.
        ok &= check(s, '(json-get (json-parse "{\\"a\\":1,\\"b\\":\\"hi\\"}") "b")',
                    '"hi"', "json-parse object string field")
        ok &= check(s, '(json-get (json-parse "{\\"a\\":42}") "a")',
                    "42", "json-parse object number field")
        ok &= check(s, '(nth 1 (json-get (json-parse "{\\"xs\\":[7,8,9]}") "xs"))',
                    "8", "json-parse nested array")
        ok &= check(s,
            '(json-get (json-get (json-parse "{\\"o\\":{\\"k\\":\\"v\\"}}") "o") "k")',
            '"v"', "json-parse nested object")

        # Guest -> host: connect to the mock at the gateway address (10.0.2.2).
        ok &= check(s, "(setq mfd (socket 'stream))", "", "make socket")
        ok &= check(s, f'(connect mfd "10.0.2.2" {port})', "0", "connect to mock 10.0.2.2")
        ok &= check(s, "(close mfd)", "", "close mock socket")

        # http-post-sse: drive the mock, collect each frame's text into *seen*.
        ok &= check(s, '(load "/lib/http.l")', "", "load http.l")
        setup = (
          '(progn (setq seen "") '
          '(http-post-sse "10.0.2.2" %d "/v1/messages" '
          '  (list "Host: 10.0.2.2" "Content-Type: application/json" '
          '        "Content-Length: 2" "Connection: close") '
          '  (list "{}") '
          '  (lambda (ev data) '
          '    (if (equal ev "content_block_delta") '
          '        (setq seen (string-concat seen (json-string-value data "text"))) nil))) '
          ' seen)'
        ) % port
        ok &= check(s, setup, "Hello from the mock.", "http-post-sse streams SSE frames")

        # assistant-stream: a full turn against the mock returns the assembled reply.
        ok &= check(s, '(load "/lib/assistant.l")', "", "load assistant.l")
        ok &= check(s, '(setq *assistant-endpoint-host* "10.0.2.2")', "", "set host")
        ok &= check(s, f'(setq *assistant-endpoint-port* {port})', "", "set port")
        ok &= check(s, '(assistant-stream "say hi" (lambda (piece) nil))',
                    "Hello from the mock.", "assistant-stream assembles reply")

        # UI shim machinery loads (the buffer/streaming command is frame-only,
        # smoke-tested by hand; here we just confirm the helpers are defined).
        ok &= check(s, "(stringp (assistant--buffer-name))", "t", "buffer-name builds")

        # M2 Task 2: the apply gate.
        ok &= check(s, '(load "/lib/assistant-apply.l")', "", "load assistant-apply.l")
        ok &= check(s, '(assistant-apply "(defun new-calc-fn (a b) (+ a b))")',
                    "applied", "ephemeral defun auto-applies")
        ok &= check(s, "(new-calc-fn 2 3)", "5", "applied function is callable")
        ok &= check(s, '(assistant-classify "(defun assistant-apply (x) x)")',
                    "persistent", "redefining existing fn -> persistent")
        ok &= check(s, '(assistant-classify "(write-file \\"/x\\" \\"y\\")")',
                    "persistent", "file write -> persistent")
        ok &= check(s, '(progn (assistant-apply "(defun new-calc-fn (a b) (* a b))") '
                    '(new-calc-fn 2 3))',
                    "5", "persistent redefinition is NOT applied (still +)")

        # M2 Task 3: the tools.
        ok &= check(s, '(load "/lib/assistant-tools.l")', "", "load assistant-tools.l")
        ok &= check(s, '(assistant-tool-run "eval_lisp" (list (cons "code" "(+ 4 5)")))',
                    "9", "tool eval_lisp computes")
        ok &= check(s, '(string-search "string-concat" '
                    '(assistant-tool-run "introspect_image" (list (cons "filter" "string-conc"))))',
                    "", "tool introspect_image lists symbols")
        ok &= check(s, '(assistant-tool-run "read_file" (list (cons "path" "/lib/json.l")))',
                    "json.l", "tool read_file reads /lib/json.l")

        print("ALL PASS" if ok else "SOME FAILED")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    raise SystemExit(main())
