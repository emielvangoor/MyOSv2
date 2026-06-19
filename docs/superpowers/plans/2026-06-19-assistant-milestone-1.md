# Assistant Milestone 1 — Proxy Proof-of-Life Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make MyOSv2 hold a streaming conversation with the real Anthropic Messages API — a prompt goes out, the assistant's reply streams back token-by-token — through a host-side TLS proxy, with **zero crypto in the OS**.

**Architecture:** Three small Lisp modules over the existing socket primitives — `json.l` (escape out, extract SSE text in), `http.l` (an HTTP/1.1 client that frames Server-Sent Events), `assistant.l` (build the request, assemble the streamed reply) — plus one tiny kernel change so `connect` accepts a literal IP (`10.0.2.2`, the proxy). Verified hermetically against a mock SSE endpoint; no token, no network.

**Tech Stack:** C (kernel `net_resolve`), the MyOSv2 Lisp dialect (Lisp-2, the `socket`/`connect`/`fd-read`/`fd-write` primitives), Python (mock endpoint + check harness over the `lisp -serve` TCP REPL).

---

## Dialect gotchas (read before writing any Lisp)

These are load-bearing — code that ignores them silently corrupts data:

1. **The reader supports `\n` `\t` `\\` `\"` but NOT `\r`** (`src/lm_core.c:476`; `\r` falls through to a literal `r`). Build CR with `(string-from-char 13)`. Never write `"\r"`.
2. **`string-concat` caps at 2048 bytes** (`src/lm_core.c:871`, fixed `char buf[2048]`). Never accumulate a large string. Write request bodies to the socket as a *list of small parts*; drain the response into *complete lines* each read (the `vterm-feed` discipline), keeping only a short fragment.
3. **`string-concat` is variadic and auto-stringifies fixnums** — `(string-concat "n=" 5)` → `"n=5"`. No `number->string` needed.
4. **`string-search` is `(string-search NEEDLE HAYSTACK)` → index or `nil`** (`user/lm_gfx.c:718`). Needle first.
5. **`string-ref` returns a character code (fixnum)**; `(string-from-char code)` makes a 1-char string.
6. **No `let*`** — nest `let`. `cond/if/progn/while/and/or/not/foreach/filter/nth/reverse` all exist.
7. **`(connect fd host port)`** resolves `host` internally; `(socket 'stream)` → fd; `(fd-read fd n)` → string or `nil` at EOF; `(fd-write fd str)` → count.
8. In `lisp -serve` mode only `bootstrap.l` + `system.l` are loaded, so tests must `(load "/lib/json.l")` etc. explicitly. Modules reach `/lib` by being listed in the Makefile's `LISP_FILES`.

## File structure

- **Create** `user/lisp/json.l` — JSON for the wire: `json-escape` (out), `json-string-value` (pull a string field out of an SSE data frame). Full recursive parse is deferred to milestone 2.
- **Create** `user/lisp/http.l` — `http-post-sse`: one POST, stream the `text/event-stream` reply, call back per SSE frame. No chunked decoding (the proxy/mock present an unchunked stream closed with `Connection: close`).
- **Create** `user/lisp/assistant.l` — `assistant-stream`: build the Messages request, drive `http-post-sse`, assemble assistant text from `text_delta` events. Plus the M1 UI shim (`*assistant-name*`, `*emiel*` buffer, `M-x emiel`).
- **Modify** `src/netstack.c` — `net_resolve` accepts a dotted-quad literal before trying DNS.
- **Modify** `src/tests.c` — a KTEST for the dotted-quad path.
- **Modify** `Makefile:38` — add `json http assistant` to `LISP_FILES`.
- **Create** `tools/mock_anthropic.py` — a canned SSE Messages endpoint for hermetic tests.
- **Create** `tools/assistant_check.py` — boots `lisp -serve`, loads the modules, asserts over TCP.

---

## Task 1: Kernel — `connect` to a literal IP (the proxy)

`net_resolve` only does DNS, so `(connect fd "10.0.2.2" 8787)` fails (it DNS-queries the string "10.0.2.2"). Teach it to recognise a dotted quad and skip DNS.

**Files:**
- Modify: `src/netstack.c:437` (top of `net_resolve`) + a new static helper above it
- Test: `src/tests.c` (new KTEST + table registration)

- [ ] **Step 1: Write the failing test**

In `src/tests.c`, add (near the other net tests, e.g. after `test_arp_resolve`):

```c
static void test_resolve_dotted_quad(void)
{
    // Pure parse path: returns before any net I/O, so no virtio_net_init needed.
    uint32_t ip = 0;
    KASSERT(net_resolve("10.0.2.2", &ip) == 0);
    KASSERT(ip == IP_GATEWAY);                 // 0x0a000202
    ip = 0;
    KASSERT(net_resolve("1.2.3.4", &ip) == 0);
    KASSERT(ip == 0x01020304u);
    ip = 0;
    KASSERT(net_resolve("999.0.0.1", &ip) != 0);   // out of range -> not a quad
}
```

Register it in the test table alongside the other `test_*` entries:

```c
    { "net: resolve dotted-quad literal", test_resolve_dotted_quad },
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test 2>&1 | grep -E "dotted-quad|FAIL|passed"`
Expected: a FAIL (or compile error: `test_resolve_dotted_quad` referencing behaviour that returns non-zero for `10.0.2.2`, because `net_resolve` currently DNS-queries it).

- [ ] **Step 3: Implement the dotted-quad fast path**

In `src/netstack.c`, add this helper immediately above `int net_resolve(...)`:

```c
// Recognise a bare IPv4 literal "a.b.c.d" (each 0..255) without DNS, so a
// caller can connect() straight to an address (e.g. the host proxy 10.0.2.2).
static int parse_dotted_quad(const char *s, uint32_t *ip)
{
    uint32_t parts[4]; int pi = 0; uint32_t cur = 0; int digits = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            cur = cur * 10u + (uint32_t)(*p - '0');
            if (cur > 255u) { return -1; }
            digits = 1;
        } else if (*p == '.' || *p == '\0') {
            if (!digits || pi >= 4) { return -1; }
            parts[pi++] = cur; cur = 0; digits = 0;
            if (*p == '\0') { break; }
        } else {
            return -1;                       // a letter -> it's a hostname
        }
    }
    if (pi != 4) { return -1; }
    *ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 0;
}
```

Then make it the first thing `net_resolve` tries — insert at the very top of the function body, before `static uint16_t qid;`:

```c
    if (parse_dotted_quad(host, ip) == 0) { return 0; }
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `make test 2>&1 | grep -E "dotted-quad|passed"`
Expected: `[PASS] net: resolve dotted-quad literal` and `==== self-tests: N passed, 0 failed ====`.

- [ ] **Step 5: Commit**

```bash
git add src/netstack.c src/tests.c
git commit -m "feat(net): net_resolve accepts a dotted-quad IP literal (connect to the proxy)"
```

---

## Task 2: `json.l` — `json-escape`

**Files:**
- Create: `user/lisp/json.l`
- Modify: `Makefile:38` (`LISP_FILES`)
- Test: `tools/assistant_check.py` (created here; pure-Lisp assertions, no mock yet)

- [ ] **Step 1: Stage the module and write the failing test**

Add `json` to `LISP_FILES` in `Makefile:38`:

```makefile
LISP_FILES  := bootstrap system modes fr-repl fr-edit fr-modes fr-keys fr-files fr-mini fr-help fr-term frame json
```

Create `tools/assistant_check.py`:

```python
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
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 tools/assistant_check.py`
Expected: `FAIL: load json.l` / errors — `json.l` does not exist yet.

- [ ] **Step 3: Create `json.l` with `json-escape`**

Create `user/lisp/json.l`:

```lisp
;;; json.l -- minimal JSON for the assistant (Phase 32.1).
;;; Milestone 1 needs only: escape a string for a request body, and pull one
;;; string field out of a small SSE data frame. A full recursive json-parse
;;; arrives in milestone 2 when tool_use inputs need structured decoding.
;;;
;;; NOTE the dialect: the reader has no \r escape and string-concat caps at
;;; 2048 bytes -- keep escaped strings short (M1 prompts are) and build CRLF
;;; from (string-from-char 13).

(defun json-escape (s)
  "S with the characters JSON requires escaped (quote, backslash, control)."
  (let ((n (string-length s)) (out "") (i 0))
    (while (< i n)
      (let ((c (string-ref s i)))
        (setq out
          (string-concat out
            (cond ((= c 34) "\\\"")          ; "
                  ((= c 92) "\\\\")          ; backslash
                  ((= c 10) "\\n")           ; LF
                  ((= c 13) "\\r")           ; CR (literal backslash-r in output)
                  ((= c 9)  "\\t")           ; TAB
                  (t (string-from-char c))))))
      (setq i (+ i 1)))
    out))
```

- [ ] **Step 4: Run it to verify it passes**

Run: `python3 tools/assistant_check.py`
Expected: `ok: load json.l`, the three `ok: json-escape ...` lines, `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add user/lisp/json.l Makefile tools/assistant_check.py
git commit -m "feat(assistant): json-escape + check harness (Phase 32.1)"
```

---

## Task 3: `json.l` — `json-string-value` (pull SSE text out)

The Messages stream sends frames like
`{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}}`.
We need the `"text"` value, with JSON escapes decoded.

**Files:**
- Modify: `user/lisp/json.l`
- Test: `tools/assistant_check.py`

- [ ] **Step 1: Write the failing test**

In `tools/assistant_check.py`, after the `json-escape` checks, add:

```python
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
            "10", "json-string-value decodes \\n")
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 tools/assistant_check.py`
Expected: FAIL on `json-string-value reads text field` (function undefined).

- [ ] **Step 3: Implement the extractor**

Append to `user/lisp/json.l`:

```lisp
(defun json-find-key (s key)
  "Index of the first content char of the string value for \"KEY\":\" in S,
or nil if absent."
  (let ((pat (string-concat "\"" key "\":\"")))
    (let ((at (string-search pat s)))
      (if at (+ at (string-length pat)) nil))))

(defun json-read-string-at (s i)
  "Decode a JSON string body starting at index I (just past the opening quote)
up to the next unescaped quote. (\\uXXXX is not decoded in M1.)"
  (let ((n (string-length s)) (out "") (done nil))
    (while (and (not done) (< i n))
      (let ((c (string-ref s i)))
        (cond
          ((= c 34) (setq done t))                     ; closing quote
          ((= c 92)                                    ; escape: take next char
           (setq i (+ i 1))
           (let ((e (if (< i n) (string-ref s i) 0)))
             (setq out (string-concat out
               (cond ((= e 110) (string-from-char 10)) ; \n
                     ((= e 116) (string-from-char 9))  ; \t
                     ((= e 114) (string-from-char 13)) ; \r
                     ((= e 34) "\"")
                     ((= e 92) "\\")
                     ((= e 47) "/")
                     (t (string-from-char e)))))))
          (t (setq out (string-concat out (string-from-char c))))))
      (setq i (+ i 1)))
    out))

(defun json-string-value (s key)
  "The decoded string value of \"KEY\" in JSON text S, or nil if absent."
  (let ((i (json-find-key s key)))
    (if i (json-read-string-at s i) nil)))
```

- [ ] **Step 4: Run it to verify it passes**

Run: `python3 tools/assistant_check.py`
Expected: the three new `ok:` lines, `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add user/lisp/json.l tools/assistant_check.py
git commit -m "feat(assistant): json-string-value extracts SSE text deltas"
```

---

## Task 4: Mock endpoint + guest→host connectivity smoke

Before HTTP, prove the guest can reach a host TCP server at `10.0.2.2` (QEMU user-net redirects `10.0.2.2` to the host loopback). This exercises Task 1 end-to-end and de-risks the whole transport.

**Files:**
- Create: `tools/mock_anthropic.py`
- Modify: `tools/assistant_check.py`

- [ ] **Step 1: Write the mock endpoint**

Create `tools/mock_anthropic.py`:

```python
#!/usr/bin/env python3
"""mock_anthropic.py -- a canned Anthropic Messages SSE endpoint for hermetic
tests. POST anything; it streams a fixed assistant reply as text_delta frames
and closes the connection (no chunked encoding, Connection: close)."""

import socket
import threading

REPLY_PIECES = ["Hello", " from", " the", " mock", "."]   # assembles to the line below
REPLY_TEXT = "".join(REPLY_PIECES)                         # "Hello from the mock."

def _sse(piece):
    body = ('{"type":"content_block_delta","index":0,'
            '"delta":{"type":"text_delta","text":"%s"}}' % piece)
    return "event: content_block_delta\r\ndata: " + body + "\r\n\r\n"

def _serve_conn(conn):
    try:
        conn.recv(65536)                       # drain the request; we ignore it
        out = "HTTP/1.1 200 OK\r\n"
        out += "Content-Type: text/event-stream\r\n"
        out += "Connection: close\r\n\r\n"
        out += "event: message_start\r\ndata: {\"type\":\"message_start\"}\r\n\r\n"
        for p in REPLY_PIECES:
            out += _sse(p)
        out += "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n"
        conn.sendall(out.encode())
    finally:
        conn.close()

def start(port=0):
    """Start the mock on 127.0.0.1:PORT (0 = pick a free port). Returns the
    bound port; serves forever on a daemon thread."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    bound = srv.getsockname()[1]

    def loop():
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=_serve_conn, args=(conn,), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()
    return bound
```

- [ ] **Step 2: Add a failing connectivity assertion**

In `tools/assistant_check.py`, import and start the mock, and add a smoke check. At the top:

```python
import mock_anthropic
```

In `main()`, before `q = boot_to_serve()`:

```python
    port = mock_anthropic.start()
    print(f"mock endpoint on 127.0.0.1:{port} (guest sees 10.0.2.2:{port})")
```

After the json checks, add:

```python
        # Guest -> host: connect to the mock at the gateway address.
        ok &= check(s, f'(setq mfd (socket (quote stream)))', "", "make socket")
        ok &= check(s, f'(connect mfd "10.0.2.2" {port})', "0", "connect to mock 10.0.2.2")
        ok &= check(s, "(close mfd)", "", "close mock socket")
```

- [ ] **Step 3: Run it to verify it fails**

Run: `python3 tools/assistant_check.py`
Expected: the `connect to mock 10.0.2.2` line is either FAIL (returns `-1`) if anything is off, or it passes — run it to learn the truth. If it FAILS with `-1`, the QEMU user-net gateway redirect is not reaching the host; fall back to adding `guestfwd=tcp:10.0.2.2:{port}-tcp:127.0.0.1:{port}` to `QEMU_NET` in `lm_harness.py`'s boot args (document it in the tool). Do not proceed until this passes.

- [ ] **Step 4: Verify it passes**

Run: `python3 tools/assistant_check.py`
Expected: `ok: connect to mock 10.0.2.2`, `ALL PASS`. (No new product code — Task 1 already enabled this; this task proves the test transport.)

- [ ] **Step 5: Commit**

```bash
git add tools/mock_anthropic.py tools/assistant_check.py
git commit -m "test(assistant): mock SSE endpoint + guest->host connectivity smoke"
```

---

## Task 5: `http.l` — `http-post-sse`

**Files:**
- Create: `user/lisp/http.l`
- Modify: `Makefile:38` (`LISP_FILES`), `tools/assistant_check.py`

- [ ] **Step 1: Stage the module and write the failing test**

Add `http` to `LISP_FILES` in `Makefile:38` (after `json`):

```makefile
LISP_FILES  := bootstrap system modes fr-repl fr-edit fr-modes fr-keys fr-files fr-mini fr-help fr-term frame json http assistant
```

(`assistant` is added now too so the next task needs no Makefile edit; the file is created in Task 6.) **Note:** the disk build `cp`s every `LISP_FILES` entry, so create an empty placeholder now to keep the build green:

```bash
printf ';;; assistant.l -- created in Task 6\n' > user/lisp/assistant.l
```

In `tools/assistant_check.py`, after `(load "/lib/json.l")`, also load http, and add a network test that drives the mock through `http-post-sse`, collecting `text_delta` data into a guest-side list:

```python
        ok &= check(s, '(load "/lib/http.l")', "", "load http.l")

        # Drive the mock: collect each SSE frame's decoded text into *seen*.
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
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 tools/assistant_check.py`
Expected: FAIL on `load http.l` (file missing).

- [ ] **Step 3: Implement `http-post-sse`**

Create `user/lisp/http.l`:

```lisp
;;; http.l -- a tiny HTTP/1.1 client that streams Server-Sent Events over the
;;; raw socket primitives (Phase 32.1). No chunked decoding: the assistant
;;; endpoint / proxy / mock present an unchunked text/event-stream and close
;;; with Connection: close, so we read until EOF.
;;;
;;; CRLF is built from (string-from-char 13) because the reader has no \r.

(defvar http-cr (string-from-char 13))
(defvar http-crlf (string-concat http-cr "\n"))

(defun http--trim-leading-space (s)
  "Drop one leading space (the SSE 'data: x' convention)."
  (if (and (> (string-length s) 0) (= (string-ref s 0) 32))
      (substring s 1 (string-length s)) s))

(defun http--strip-cr (s)
  "Drop a single trailing CR from a line."
  (if (and (> (string-length s) 0)
           (= (string-ref s (- (string-length s) 1)) 13))
      (substring s 0 (- (string-length s) 1)) s))

(defun http--dispatch (ev data on-event)
  (if (> (string-length data) 0) (on-event ev data) nil))

(defun http-post-sse (host port path header-lines body-parts on-event)
  "POST to HOST:PORT PATH; HEADER-LINES are \"Key: value\" strings, BODY-PARTS a
list of strings (never one big string -> dodges the 2048 cap). Parse the
text/event-stream reply, calling (ON-EVENT event-type data-json) per frame.
Returns t on a clean finish, nil if the socket could not be opened."
  (let ((fd (socket 'stream)))
    (if (< fd 0) nil
      (if (< (connect fd host port) 0) (progn (close fd) nil)
        (progn
          (fd-write fd (string-concat "POST " path " HTTP/1.1" http-crlf))
          (foreach (lambda (h) (fd-write fd (string-concat h http-crlf))) header-lines)
          (fd-write fd http-crlf)
          (foreach (lambda (p) (fd-write fd p)) body-parts)
          (http--read-stream fd on-event)
          (close fd)
          t)))))

(defun http--read-stream (fd on-event)
  "Read FD to EOF, skip the response headers, frame SSE lines, dispatch frames."
  (let ((frag "") (in-body nil) (ev "message") (data "") (go t))
    (while go
      (let ((chunk (fd-read fd 4096)))
        (if (not chunk) (setq go nil)
          (progn
            (setq frag (string-concat frag chunk))
            (if (not in-body)
                (let ((b (string-search (string-concat http-crlf http-crlf) frag)))
                  (if b (progn
                          (setq frag (substring frag (+ b 4) (string-length frag)))
                          (setq in-body t))
                    nil))
              nil)
            (if in-body
                (let ((nl (string-search "\n" frag)))
                  (while nl
                    (let ((line (http--strip-cr (substring frag 0 nl))))
                      (setq frag (substring frag (+ nl 1) (string-length frag)))
                      (cond
                        ((= (string-length line) 0)
                         (http--dispatch ev data on-event)
                         (setq ev "message") (setq data ""))
                        ((and (>= (string-length line) 6)
                              (equal (substring line 0 6) "event:"))
                         (setq ev (http--trim-leading-space
                                    (substring line 6 (string-length line)))))
                        ((and (>= (string-length line) 5)
                              (equal (substring line 0 5) "data:"))
                         (setq data (http--trim-leading-space
                                      (substring line 5 (string-length line)))))
                        (t nil)))
                    (setq nl (string-search "\n" frag))))
              nil)))))
    (http--dispatch ev data on-event)))   ; flush a final frame with no blank line
```

- [ ] **Step 4: Run it to verify it passes**

Run: `python3 tools/assistant_check.py`
Expected: `ok: http-post-sse streams SSE frames` (the callback assembled `"Hello from the mock."`), `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add user/lisp/http.l user/lisp/assistant.l Makefile tools/assistant_check.py
git commit -m "feat(assistant): http-post-sse streams Server-Sent Events over sockets"
```

---

## Task 6: `assistant.l` — `assistant-stream`

Build a real Messages request and assemble the streamed reply.

**Files:**
- Modify: `user/lisp/assistant.l`, `tools/assistant_check.py`

- [ ] **Step 1: Write the failing test**

In `tools/assistant_check.py`, after the http check, add:

```python
        ok &= check(s, '(load "/lib/assistant.l")', "", "load assistant.l")
        # Point the assistant at the mock and run one turn; the returned string
        # is the assembled assistant reply.
        ok &= check(s, f'(setq *assistant-endpoint-host* "10.0.2.2")', "", "set host")
        ok &= check(s, f'(setq *assistant-endpoint-port* {port})', "", "set port")
        ok &= check(s, '(assistant-stream "say hi" (lambda (piece) nil))',
                    "Hello from the mock.", "assistant-stream assembles reply")
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 tools/assistant_check.py`
Expected: FAIL on `assistant-stream assembles reply` (function undefined; the file is still the placeholder).

- [ ] **Step 3: Implement `assistant-stream`**

Replace `user/lisp/assistant.l` with:

```lisp
;;; assistant.l -- the native agent's request path (Phase 32.1, proof of life).
;;; Builds an Anthropic Messages request, drives http-post-sse, and assembles
;;; the streamed assistant text. The tool loop, the apply gate and persistence
;;; arrive in milestone 2.

(defvar *assistant-name* "emiel")              ; overridable persona name
(defvar *assistant-endpoint-host* "10.0.2.2")  ; proxy by default (M1)
(defvar *assistant-endpoint-port* 8787)
(defvar *assistant-model* "claude-sonnet-4-6")
(defvar *assistant-max-tokens* 1024)

(defun assistant--api-key ()
  "Read the API key from /lib/claude/api-key (one line), or \"\" if absent."
  (let ((fd (open "/lib/claude/api-key")))
    (if (< fd 0) ""
      (let ((k (fd-read fd 256)))
        (close fd)
        (if k (assistant--first-line k) "")))))

(defun assistant--first-line (s)
  (let ((nl (string-search "\n" s)))
    (if nl (substring s 0 nl) s)))

(defun assistant--request-body (prompt)
  "A list of body parts (never one big string) for a single-user-turn request."
  (list
    (string-concat
      "{\"model\":\"" *assistant-model* "\","
      "\"max_tokens\":" *assistant-max-tokens* ","
      "\"stream\":true,"
      "\"messages\":[{\"role\":\"user\",\"content\":\"")
    (json-escape prompt)
    "\"}]}"))

(defun assistant-stream (prompt on-piece)
  "Send PROMPT as one user turn; stream the reply, calling (ON-PIECE text) for
each text_delta, and return the full assembled assistant text."
  (let ((body (assistant--request-body prompt)) (acc ""))
    (let ((blen 0))
      (foreach (lambda (p) (setq blen (+ blen (string-length p)))) body)
      (http-post-sse
        *assistant-endpoint-host* *assistant-endpoint-port* "/v1/messages"
        (list (string-concat "Host: " *assistant-endpoint-host*)
              "Content-Type: application/json"
              "anthropic-version: 2023-06-01"
              (string-concat "x-api-key: " (assistant--api-key))
              (string-concat "Content-Length: " blen)
              "Connection: close")
        body
        (lambda (ev data)
          (if (equal ev "content_block_delta")
              (let ((piece (json-string-value data "text")))
                (if piece (progn (setq acc (string-concat acc piece))
                                 (on-piece piece)) nil))
            nil))))
    acc))
```

- [ ] **Step 4: Run it to verify it passes**

Run: `python3 tools/assistant_check.py`
Expected: `ok: assistant-stream assembles reply`, `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add user/lisp/assistant.l tools/assistant_check.py
git commit -m "feat(assistant): assistant-stream builds the Messages request, assembles the reply"
```

---

## Task 7: UI shim — `M-x emiel` streams into `*emiel*`

The headless core is proven; now wire a thin frame command. This is smoke-tested by hand (a frame feature; the network path is already covered by Task 6).

**Files:**
- Modify: `user/lisp/assistant.l`

- [ ] **Step 1: Add the buffer-streaming command**

Append to `user/lisp/assistant.l`:

```lisp
;;; ---- M1 UI shim: M-x emiel -> stream a reply into the *emiel* buffer -------
;;; The dynamic rename (set-assistant-name re-aliasing M-x) lands in milestone 4
;;; once the function-cell primitive is confirmed; M1 ships the default command.

(defun assistant--buffer-name () (string-concat "*" *assistant-name* "*"))

(defun assistant-ask (prompt)
  "Open the assistant buffer and stream a reply to PROMPT into it."
  (let ((b (make-buffer (assistant--buffer-name))))
    (set-buffer b)
    (goto-char (buffer-length))
    (insert (string-concat "\n> " prompt "\n"))
    (insert (string-concat *assistant-name* ": "))
    (redisplay)
    (assistant-stream prompt
      (lambda (piece)
        (set-buffer b) (goto-char (buffer-length))
        (insert piece) (redisplay)))
    (set-buffer b) (goto-char (buffer-length)) (insert "\n") (redisplay)))

(defun emiel ()
  "M-x emiel: ask the assistant; its reply streams into the *emiel* buffer."
  (interactive)
  (mb-start 'assistant-ask))   ; minibuffer reads the prompt, calls assistant-ask
```

**Note for the implementer:** confirm `make-buffer` returns a buffer handle and `mb-start` accepts a callback symbol (see `user/lisp/fr-files.l:23` for the `mb-start` pattern and `user/lisp/fr-term.l` for `make-buffer`/`set-buffer`/`insert`/`goto-char`/`buffer-length` usage). Adjust the call shape to match those exactly if they differ.

- [ ] **Step 2: Wire it to load in the frame**

`assistant.l` is in `LISP_FILES`, but the frame loads its modules explicitly. In `user/lisp/frame.l`, beside the existing `(load "/lib/fr-term.l")`, add:

```lisp
(load "/lib/json.l")
(load "/lib/http.l")
(load "/lib/assistant.l")
```

- [ ] **Step 3: Build and smoke-test by hand**

Run: `make build/kernel.elf build/user/vterm.elf && make build/disk.img`
Then boot the GUI, run a host TLS proxy (or the mock) on `10.0.2.2:<port>`, set `*assistant-endpoint-port*`, and `M-x emiel`. Expected: a `*emiel*` buffer opens and the reply streams in after `emiel: `.

For a headless smoke (no frame), confirm `(emiel)`'s machinery loads without error:

Run: `python3 tools/assistant_check.py` (already loads `assistant.l`; add `ok &= check(s, "(stringp (assistant--buffer-name))", "t", "buffer-name builds")`).
Expected: `ALL PASS`.

- [ ] **Step 4: Commit**

```bash
git add user/lisp/assistant.l user/lisp/frame.l tools/assistant_check.py
git commit -m "feat(assistant): M-x emiel streams a reply into the *emiel* buffer (M1 shim)"
```

---

## Task 8: Document the host proxy (so a human can talk to the real API)

M1 is hermetic against the mock; this records how to point it at the *real* API through a host TLS proxy, for live use.

**Files:**
- Create: `docs/notes/phase-32.md`

- [ ] **Step 1: Write the note**

Create `docs/notes/phase-32.md`:

```markdown
# Phase 32 notes

## 32.1 — proof of life (proxy)

The assistant speaks plaintext HTTP to a host-side TLS proxy that forwards to
`https://api.anthropic.com`. From the guest, the host is `10.0.2.2`.

Run a proxy on the host (example with socat):

    socat TCP-LISTEN:8787,reuseaddr,fork \
          OPENSSL:api.anthropic.com:443

Then in the OS:

    (setq *assistant-endpoint-host* "10.0.2.2")
    (setq *assistant-endpoint-port* 8787)
    ;; put your key in /lib/claude/api-key (one line)
    M-x emiel

Caveats (accepted for a hobby OS, see the spec): the API key is plaintext on
disk; the proxy presents an unchunked stream (socat OPENSSL passes the API's
chunked encoding through verbatim, so for the *real* endpoint add chunked
decoding to http.l, or use a proxy that de-chunks). Hermetic tests use
`tools/mock_anthropic.py`, which is already unchunked.
```

- [ ] **Step 2: Commit**

```bash
git add docs/notes/phase-32.md
git commit -m "docs(phase-32): host TLS proxy setup for the live assistant"
```

---

## Self-review notes

- **Spec coverage (milestone 1):** `http.l` + `json.l` + `assistant-stream` streaming into `*emiel*` via the proxy — Tasks 2,3,5,6,7. Hermetic mock check — Tasks 4,6 (`tools/assistant_check.py` + `tools/mock_anthropic.py`). The "no big-string accumulation" rule — body sent as parts (Task 5/6), response drained per-line (Task 5). Dotted-quad to reach `10.0.2.2` — Task 1.
- **Known M1 scope cuts (deferred, documented):** full recursive `json-parse` → M2 (M1 uses the targeted extractor); chunked-transfer decoding → noted in Task 8 (mock is unchunked); `\uXXXX` decoding → noted in Task 3; dynamic `M-x` rename via `set-assistant-name` → M4 (M1 ships the default `emiel` command); `json-escape`/extractor inherit the 2048-byte cap → fine for short M1 prompts, flagged in `json.l`.
- **Type consistency:** `http-post-sse` signature `(host port path header-lines body-parts on-event)` is identical in Task 5 (def), Task 5 (test), and Task 6 (caller). `json-string-value`/`json-escape`/`assistant-stream` names match across tasks. CRLF is `http-crlf` everywhere.
- **Verification:** `make test` (Task 1 KTEST) + `python3 tools/assistant_check.py` (Tasks 2–7) are the green bar; both are token-free and hermetic.
```
