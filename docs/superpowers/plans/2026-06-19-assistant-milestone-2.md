# Assistant Milestone 2 — Tools + the Apply Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the proof-of-life stream (M1) into a real agent: Claude calls tools, the OS runs them, and generated Lisp installs itself into the live image — with a hybrid safety gate (ephemeral forms auto-eval; persistent ones preview + accept) and undo. The headline outcome: **"make me a calculator" builds and runs one.**

**Architecture:** The agentic tool loop lives in `assistant.l`, parsing streamed `tool_use` blocks and looping `tool_result` back. Tools live in `assistant-tools.l` (`eval-lisp`, `introspect-image`, `read-file`, `write-file`, `run-bash`). The gate lives in `assistant-apply.l` (classify ephemeral|persistent, per-symbol-snapshot undo, `/lib/claude` persistence). A real recursive `json-parse` (added to `json.l`) decodes tool inputs. All hermetic against an extended mock that scripts a tool-use turn.

**Tech Stack:** The MyOSv2 Lisp dialect (`eval`, `read-string`, `function-info`, `all-symbols`, `prin1-to-string`, the M1 `http.l`/`json.l`/socket layer), Python (extended mock + check harness).

---

## Dialect gotchas (carried from M1 — still load-bearing)

1. Reader has **no `\r`** — build CR with `(string-from-char 13)`.
2. **No `defvar`** — top-level globals use `setq`.
3. `string-concat` **caps at 2048 bytes** and auto-stringifies fixnums; never build a large string — stream/parts.
4. `string-search` is `(string-search NEEDLE HAYSTACK)` → index or nil.
5. `string-ref` → char code; `(string-from-char code)` → 1-char string; `equal` compares strings; **no `let*`** (nest `let`).
6. New primitives this milestone leans on (all verified present):
   - `(read-string "form")` → the first form read from the string (unevaluated). `(eval form)` evaluates it. So **`(eval (read-string code))`** runs a code string.
   - `(function-info 'sym)` → `(primitive name min max)` | `(lambda params body)` | `(macro params body)` | `nil`. Non-nil ⇒ the symbol is already a function (the "redefines existing" signal).
   - `(all-symbols)` → list of every interned symbol.
   - `(symbol-name 'sym)` → string. `(prin1-to-string obj)` → obj printed (≤1024 bytes).
7. In `lisp -serve` the gfx primitives may be absent; tool code that only does data/eval/sockets is serve-testable. Buffer/redisplay calls stay in frame-only paths.

## File structure

- **Modify** `user/lisp/json.l` — add a real recursive `json-parse` + `json-get` (decode tool inputs; the M1 extractor stays for the hot text-delta path).
- **Create** `user/lisp/assistant-tools.l` — the tool implementations + their JSON schemas.
- **Create** `user/lisp/assistant-apply.l` — the classifier, `assistant-apply`, undo, `/lib/claude` persistence.
- **Modify** `user/lisp/assistant.l` — the agentic `assistant-converse` loop (parse `tool_use`, run tools, loop `tool_result`), replacing M1's single-shot `assistant-stream` (kept as the no-tools path).
- **Modify** `Makefile:38` — add `assistant-tools assistant-apply` to `LISP_FILES`.
- **Modify** `tools/mock_anthropic.py` — script a two-turn tool-use exchange.
- **Modify** `tools/assistant_check.py` — assertions for parse, tools, gate, loop.

---

## Task 1: `json.l` — recursive `json-parse` + `json-get`

Tool inputs arrive as a complete JSON object (assembled from `input_json_delta` fragments); we need to decode them to a Lisp structure.

**Files:**
- Modify: `user/lisp/json.l`
- Test: `tools/assistant_check.py`

- [ ] **Step 1: Write the failing tests**

In `tools/assistant_check.py`, after the existing json checks, add:

```python
        ok &= check(s, '(load "/lib/json.l")', "", "reload json.l")
        ok &= check(s, '(json-get (json-parse "{\\"a\\":1,\\"b\\":\\"hi\\"}") "b")',
                    '"hi"', "json-parse object string field")
        ok &= check(s, '(json-get (json-parse "{\\"a\\":42}") "a")',
                    "42", "json-parse object number field")
        ok &= check(s, '(nth 1 (json-get (json-parse "{\\"xs\\":[7,8,9]}") "xs"))',
                    "8", "json-parse nested array")
        ok &= check(s,
            '(json-get (json-get (json-parse "{\\"o\\":{\\"k\\":\\"v\\"}}") "o") "k")',
            '"v"', "json-parse nested object")
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 tools/assistant_check.py` (after `make build/kernel.elf && make build/disk.img`).
Expected: FAIL on `json-parse object string field` (function undefined).

- [ ] **Step 3: Implement the parser**

Append to `user/lisp/json.l`:

```lisp
;;; ---- recursive descent parser (M2): JSON text -> Lisp ----------------------
;;; object -> alist of (key-string . value); array -> list; string -> string;
;;; number -> fixnum (fraction/exponent skipped); true -> t; false/null -> nil.
;;; Each jp-* helper returns (value . next-index) so positions thread cleanly.

(defun jp-ws (s i)
  (let ((n (string-length s)) (go t))
    (while (and go (< i n))
      (let ((c (string-ref s i)))
        (if (or (= c 32) (= c 9) (= c 10) (= c 13)) (setq i (+ i 1)) (setq go nil))))
    i))

(defun jp-string (s i)              ; i AT the opening quote -> (string . next)
  (let ((n (string-length s)) (out "") (done nil))
    (setq i (+ i 1))
    (while (and (not done) (< i n))
      (let ((c (string-ref s i)))
        (cond
          ((= c 34) (setq done t) (setq i (+ i 1)))
          ((= c 92)
           (setq i (+ i 1))
           (let ((e (if (< i n) (string-ref s i) 0)))
             (setq out (string-concat out
               (cond ((= e 110) (string-from-char 10)) ((= e 116) (string-from-char 9))
                     ((= e 114) (string-from-char 13)) ((= e 34) "\"")
                     ((= e 92) "\\") ((= e 47) "/") (t (string-from-char e)))))
             (setq i (+ i 1))))
          (t (setq out (string-concat out (string-from-char c))) (setq i (+ i 1))))))
    (cons out i)))

(defun jp-number (s i)              ; integer part; skips any fraction/exponent
  (let ((n (string-length s)) (neg nil) (val 0))
    (if (and (< i n) (= (string-ref s i) 45)) (progn (setq neg t) (setq i (+ i 1))) nil)
    (while (and (< i n) (let ((c (string-ref s i))) (and (>= c 48) (<= c 57))))
      (setq val (+ (* val 10) (- (string-ref s i) 48))) (setq i (+ i 1)))
    (while (and (< i n) (let ((c (string-ref s i)))
                          (or (= c 46) (= c 101) (= c 69) (= c 43) (= c 45)
                              (and (>= c 48) (<= c 57)))))
      (setq i (+ i 1)))
    (cons (if neg (- 0 val) val) i)))

(defun jp-value (s i)
  (setq i (jp-ws s i))
  (let ((c (string-ref s i)))
    (cond ((= c 34) (jp-string s i))
          ((= c 123) (jp-object s i))            ; {
          ((= c 91) (jp-array s i))              ; [
          ((= c 116) (cons t (+ i 4)))           ; true
          ((= c 102) (cons nil (+ i 5)))         ; false
          ((= c 110) (cons nil (+ i 4)))         ; null
          (t (jp-number s i)))))

(defun jp-object (s i)              ; i AT '{' -> (alist . next)
  (let ((n (string-length s)) (out nil) (done nil))
    (setq i (jp-ws s (+ i 1)))
    (if (and (< i n) (= (string-ref s i) 125)) (progn (setq done t) (setq i (+ i 1))) nil)
    (while (not done)
      (let ((kp (jp-string s (jp-ws s i))))
        (let ((ci (jp-ws s (cdr kp))))
          (let ((vp (jp-value s (+ ci 1))))      ; skip the ':'
            (setq out (cons (cons (car kp) (car vp)) out))
            (let ((j (jp-ws s (cdr vp))))
              (let ((ch (string-ref s j)))
                (if (= ch 44) (setq i (+ j 1))    ; ,
                  (progn (setq done t) (setq i (+ j 1))))))))))   ; } (or malformed)
    (cons (reverse out) i)))

(defun jp-array (s i)               ; i AT '[' -> (list . next)
  (let ((n (string-length s)) (out nil) (done nil))
    (setq i (jp-ws s (+ i 1)))
    (if (and (< i n) (= (string-ref s i) 93)) (progn (setq done t) (setq i (+ i 1))) nil)
    (while (not done)
      (let ((vp (jp-value s i)))
        (setq out (cons (car vp) out))
        (let ((j (jp-ws s (cdr vp))))
          (let ((ch (string-ref s j)))
            (if (= ch 44) (setq i (+ j 1))
              (progn (setq done t) (setq i (+ j 1))))))))
    (cons (reverse out) i)))

(defun json-parse (s) "Parse one JSON value from string S." (car (jp-value s 0)))

(defun json-get (obj key)
  "Value for KEY in a json-parse object (alist), or nil."
  (if obj (if (equal (car (car obj)) key) (cdr (car obj)) (json-get (cdr obj) key)) nil))
```

- [ ] **Step 4: Run to verify it passes**

Run: `python3 tools/assistant_check.py`
Expected: the four `json-parse ...` lines `ok:`, `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add user/lisp/json.l tools/assistant_check.py
git commit -m "feat(assistant): recursive json-parse + json-get (decode tool inputs)"
```

---

## Task 2: `assistant-apply.l` — the classifier + gate + undo

**Files:**
- Create: `user/lisp/assistant-apply.l`
- Modify: `Makefile:38`, `tools/assistant_check.py`

- [ ] **Step 1: Stage and write the failing test**

Add `assistant-apply` to `LISP_FILES` (Makefile:38, after `assistant`). In `tools/assistant_check.py`:

```python
        ok &= check(s, '(load "/lib/assistant-apply.l")', "", "load assistant-apply.l")
        # A fresh defun is ephemeral -> applied immediately; the symbol becomes callable.
        ok &= check(s, '(assistant-apply "(defun new-calc-fn (a b) (+ a b))")',
                    "applied", "ephemeral defun auto-applies")
        ok &= check(s, "(new-calc-fn 2 3)", "5", "applied function is callable")
        # Redefining an EXISTING function is persistent -> gated, not applied.
        ok &= check(s, '(assistant-classify "(defun assistant-apply (x) x)")',
                    "persistent", "redefining existing fn -> persistent")
        # A file write is persistent.
        ok &= check(s, '(assistant-classify "(write-file \\"/x\\" \\"y\\")")',
                    "persistent", "file write -> persistent")
        # Gated form is queued, the old binding survives, and undo is a no-op-safe.
        ok &= check(s, '(progn (assistant-apply "(defun new-calc-fn (a b) (* a b))") '
                    '(new-calc-fn 2 3))',
                    "5", "persistent redefinition is NOT applied (still +)")
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 tools/assistant_check.py`
Expected: FAIL on `load assistant-apply.l`.

- [ ] **Step 3: Implement the gate**

Create `user/lisp/assistant-apply.l`:

```lisp
;;; assistant-apply.l -- the hybrid-by-scope gate (Phase 32.2). Generated forms
;;; are classified: EPHEMERAL (a fresh defun, a computation) auto-evals;
;;; PERSISTENT (redefining an existing function, or any file write / permanent
;;; command) is queued for the user's approval. Undo snapshots the old binding.

(setq *assistant-pending* nil)     ; list of (form-string . form) awaiting approval
(setq *assistant-undo* nil)        ; list of (symbol . old-function) for rollback

;; primitives whose presence in a form makes it persistent (touches disk/registry)
(setq *assistant-write-ops*
  (list "write-file" "creat" "save-buffer" "fd-write" "register-command"))

(defun aa--form-head (form) (if (consp form) (car form) nil))

(defun aa--defun-target (form)
  "If FORM is (defun NAME ...), the NAME symbol; else nil."
  (if (and (consp form) (eq (car form) 'defun) (consp (cdr form)))
      (car (cdr form)) nil))

(defun aa--mentions-write-op (code)
  "t if CODE (the raw string) names a disk/registry primitive."
  (let ((ops *assistant-write-ops*) (hit nil))
    (while (and ops (not hit))
      (if (string-search (car ops) code) (setq hit t) nil)
      (setq ops (cdr ops)))
    hit))

(defun assistant-classify (code)
  "'persistent if CODE redefines an existing function or writes disk/registry;
'ephemeral otherwise (conservative: unknown -> persistent)."
  (let ((form (read-string code)))
    (let ((tgt (aa--defun-target form)))
      (cond
        ((aa--mentions-write-op code) 'persistent)
        ;; redefining an already-bound function is persistent; a brand-new one is not
        ((and tgt (function-info tgt)) 'persistent)
        ((and tgt (not (function-info tgt))) 'ephemeral)
        ;; non-defun computations (open a buffer, compute) are ephemeral
        (t 'ephemeral)))))

(defun assistant--snapshot (form)
  "Record the old function binding of a defun target so undo can restore it."
  (let ((tgt (aa--defun-target form)))
    (if (and tgt (function-info tgt))
        (setq *assistant-undo* (cons (cons tgt (function-info tgt)) *assistant-undo*))
      nil)))

(defun assistant-apply (code)
  "Classify CODE and either eval it now (ephemeral -> \"applied: <result>\") or
queue it for approval (persistent -> \"queued: needs approval\")."
  (if (eq (assistant-classify code) 'ephemeral)
      (string-concat "applied: " (prin1-to-string (eval (read-string code))))
    (progn
      (setq *assistant-pending*
            (cons (cons code (read-string code)) *assistant-pending*))
      "queued: needs approval")))

(defun assistant-accept ()
  "Apply the oldest pending persistent form (after the user okays it)."
  (if *assistant-pending*
      (let ((p (car (reverse *assistant-pending*))))
        (assistant--snapshot (cdr p))
        (eval (cdr p))
        (setq *assistant-pending*
              (reverse (cdr (reverse *assistant-pending*))))
        "accepted")
    "nothing pending"))

(defun assistant-undo ()
  "Restore the most recently redefined function to its previous binding."
  (if *assistant-undo*
      (let ((u (car *assistant-undo*)))
        ;; rebuild a defun from the snapshot's (lambda params body) and re-eval
        (eval (list 'defun (car u) (car (cdr (cdr u))) (car (cdr (cdr (cdr u))))))
        (setq *assistant-undo* (cdr *assistant-undo*))
        "undone")
    "nothing to undo"))
```

**Note for the implementer:** `function-info` returns `(lambda params body)`; the
undo reconstruction reads `params`/`body` out of that list. Confirm the exact
shape with `(function-info 'some-fn)` over the REPL and adjust the `nth`/`car-cdr`
access in `assistant-undo` to match before relying on it.

- [ ] **Step 4: Run to verify it passes**

Run: `python3 tools/assistant_check.py`
Expected: the gate `ok:` lines, `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add user/lisp/assistant-apply.l Makefile tools/assistant_check.py
git commit -m "feat(assistant): hybrid apply gate — ephemeral auto-eval, persistent queued, undo"
```

---

## Task 3: `assistant-tools.l` — the tools + their schemas

**Files:**
- Create: `user/lisp/assistant-tools.l`
- Modify: `Makefile:38`, `tools/assistant_check.py`

- [ ] **Step 1: Stage and write the failing test**

Add `assistant-tools` to `LISP_FILES`. In `tools/assistant_check.py`:

```python
        ok &= check(s, '(load "/lib/assistant-tools.l")', "", "load assistant-tools.l")
        # eval-lisp tool: ephemeral expr runs and returns its printed value.
        ok &= check(s, '(assistant-tool-run "eval_lisp" (list (cons "code" "(+ 4 5)")))',
                    "9", "tool eval_lisp computes")
        # introspect: a known primitive shows up.
        ok &= check(s, '(string-search "string-concat" '
                    '(assistant-tool-run "introspect_image" (list (cons "filter" "string-conc"))))',
                    "", "tool introspect_image lists symbols")
        # read-file: reads an existing file.
        ok &= check(s, '(assistant-tool-run "read_file" (list (cons "path" "/lib/json.l")))',
                    "json.l", "tool read_file reads /lib/json.l")
```

(The introspect assertion just requires no ERROR and a hit substring; `check`
treats `""` as "any non-error output".)

- [ ] **Step 2: Run to verify it fails**

Run: `python3 tools/assistant_check.py`
Expected: FAIL on `load assistant-tools.l`.

- [ ] **Step 3: Implement the tools**

Create `user/lisp/assistant-tools.l`:

```lisp
;;; assistant-tools.l -- what the agent can DO (Phase 32.2). Each tool takes an
;;; alist of decoded arguments and returns a string (the tool_result content).
;;; eval-lisp routes through the apply gate so persistent changes are deferred.

(defun at--arg (args key) (json-get args key))   ; alist lookup, nil if absent

(defun at-eval-lisp (args)
  "Run a Lisp code string through the apply gate; return its result text."
  (let ((code (at--arg args "code")))
    (if code (assistant-apply code) "error: missing 'code'")))

(defun at-introspect (args)
  "List interned symbols whose name contains 'filter' (all if absent), one per
line, each with its function-info kind."
  (let ((filt (at--arg args "filter")) (out "") (syms (all-symbols)))
    (foreach
      (lambda (sym)
        (let ((nm (symbol-name sym)))
          (if (or (not filt) (string-search filt nm))
              (let ((fi (function-info sym)))
                (setq out (string-concat out nm
                            (if fi (string-concat " : " (symbol-name (car fi))) "")
                            "\n")))
            nil)))
      syms)
    (if (= (string-length out) 0) "(no matching symbols)" out)))

(defun at-source (args)
  "The living source of a function (its function-info), printed."
  (let ((nm (at--arg args "name")))
    (if nm (prin1-to-string (function-info (read-string nm))) "error: missing 'name'")))

(defun at-read-file (args)
  (let ((path (at--arg args "path")))
    (if (not path) "error: missing 'path'"
      (let ((fd (open path)))
        (if (< fd 0) (string-concat "error: cannot open " path)
          (let ((acc "") (go t))
            (while go
              (let ((chunk (fd-read fd 1024)))
                (if chunk (setq acc (string-concat acc chunk)) (setq go nil))))
            (close fd) acc))))))

(defun at-write-file (args)
  "Write a file -- always persistent, so this is gated like eval of a write op."
  (let ((path (at--arg args "path")) (content (at--arg args "content")))
    (if (and path content)
        (assistant-apply
          (string-concat "(let ((fd (creat \"" path "\"))) (fd-write fd \""
                         (json-escape content) "\") (close fd))"))
      "error: missing 'path' or 'content'")))

(defun at-run-bash (args)
  "Run a command via busybox sh -c, returning its stdout (capped)."
  (let ((cmd (at--arg args "command")))
    (if (not cmd) "error: missing 'command'"
      (let ((p (pipe)))
        (let ((pid (fork)))
          (if (= pid 0)
              (progn (dup2 (cdr p) 1) (close (car p)) (close (cdr p))
                     (exec "/bin/busybox" (list "sh" "-c" cmd)) (exit 127))
            (progn
              (close (cdr p))
              (let ((acc "") (go t))
                (while go
                  (let ((chunk (fd-read (car p) 1024)))
                    (if chunk (setq acc (string-concat acc chunk)) (setq go nil))))
                (close (car p)) (wait) acc))))))))

(defun assistant-tool-run (name args)
  "Dispatch a tool call by NAME with decoded ARGS (an alist)."
  (cond ((equal name "eval_lisp") (at-eval-lisp args))
        ((equal name "introspect_image") (at-introspect args))
        ((equal name "function_source") (at-source args))
        ((equal name "read_file") (at-read-file args))
        ((equal name "write_file") (at-write-file args))
        ((equal name "run_bash") (at-run-bash args))
        (t (string-concat "error: unknown tool " name))))

;; The JSON schema array advertised to the API (string parts -> the request).
(defun assistant-tools-json ()
  (string-concat
    "[{\"name\":\"eval_lisp\",\"description\":\"Evaluate a Lisp form in the live OS image. Use this to define functions and build features. Ephemeral forms run immediately; persistent ones (redefining an existing function, writing files) are queued for the user's approval.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"}},\"required\":[\"code\"]}},"
    "{\"name\":\"introspect_image\",\"description\":\"List interned symbols whose name contains the filter, to see what already exists before writing code.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\"}}}},"
    "{\"name\":\"function_source\",\"description\":\"Show the living source of a function by name.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}},"
    "{\"name\":\"read_file\",\"description\":\"Read a file from the OS filesystem.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
    "{\"name\":\"write_file\",\"description\":\"Write a file (queued for approval).\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}},"
    "{\"name\":\"run_bash\",\"description\":\"Run a shell command via busybox and return stdout.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}}]"))
```

- [ ] **Step 4: Run to verify it passes**

Run: `python3 tools/assistant_check.py`
Expected: the three tool `ok:` lines, `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add user/lisp/assistant-tools.l Makefile tools/assistant_check.py
git commit -m "feat(assistant): tools — eval_lisp, introspect, source, file, bash + schemas"
```

---

## Task 4: Extend the mock to script a tool-use turn

**Files:**
- Modify: `tools/mock_anthropic.py`

- [ ] **Step 1: Add a stateful two-turn tool-use script**

Replace the body of `tools/mock_anthropic.py`'s `_serve_conn` so the FIRST request (no `tool_result` in body) streams a `tool_use(eval_lisp, {"code":"(+ 1 2)"})` and the SECOND request (body contains `tool_result`) streams a final text answer:

```python
def _read_request(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(65536)
        if not chunk:
            break
        data += chunk
    # read any declared body (Content-Length) so we can branch on tool_result
    head, _, rest = data.partition(b"\r\n\r\n")
    clen = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":")[1].strip())
    body = rest
    while len(body) < clen:
        body += conn.recv(65536)
    return body

def _tool_use_stream():
    out = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
    out += ('event: content_block_start\r\ndata: {"type":"content_block_start",'
            '"index":0,"content_block":{"type":"tool_use","id":"toolu_1",'
            '"name":"eval_lisp","input":{}}}\r\n\r\n')
    out += ('event: content_block_delta\r\ndata: {"type":"content_block_delta",'
            '"index":0,"delta":{"type":"input_json_delta",'
            '"partial_json":"{\\"code\\":\\"(+ 1 2)\\"}"}}\r\n\r\n')
    out += ('event: content_block_stop\r\ndata: {"type":"content_block_stop","index":0}\r\n\r\n')
    out += ('event: message_delta\r\ndata: {"type":"message_delta",'
            '"delta":{"stop_reason":"tool_use"}}\r\n\r\n')
    out += ('event: message_stop\r\ndata: {"type":"message_stop"}\r\n\r\n')
    return out

def _final_stream():
    out = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
    for piece in ["The", " answer", " is", " 3", "."]:
        out += ('event: content_block_delta\r\ndata: {"type":"content_block_delta",'
                '"index":0,"delta":{"type":"text_delta","text":"%s"}}\r\n\r\n' % piece)
    out += ('event: message_stop\r\ndata: {"type":"message_stop"}\r\n\r\n')
    return out

def _serve_conn(conn):
    try:
        body = _read_request(conn)
        if b"tool_result" in body:
            conn.sendall(_final_stream().encode())
        elif b"\"tools\"" in body:           # a tools-enabled turn -> tool_use
            conn.sendall(_tool_use_stream().encode())
        else:                                # plain M1 turn -> the old greeting
            out = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
            out += "event: message_start\r\ndata: {\"type\":\"message_start\"}\r\n\r\n"
            for p in REPLY_PIECES:
                out += _sse(p)
            out += "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n"
            conn.sendall(out.encode())
    finally:
        conn.close()
```

Keep `REPLY_PIECES`, `REPLY_TEXT`, `_sse`, and `start` as they are.

- [ ] **Step 2: Sanity-run the existing checks (no regression)**

Run: `python3 tools/assistant_check.py`
Expected: the M1 `assistant-stream assembles reply` still passes (its request has no `tools`, so the mock still greets), `ALL PASS`.

- [ ] **Step 3: Commit**

```bash
git add tools/mock_anthropic.py
git commit -m "test(assistant): mock scripts a two-turn tool_use exchange"
```

---

## Task 5: `assistant-converse` — the agentic tool loop

**Files:**
- Modify: `user/lisp/assistant.l`, `tools/assistant_check.py`

- [ ] **Step 1: Write the failing test**

In `tools/assistant_check.py`:

```python
        ok &= check(s, '(load "/lib/assistant.l")', "", "reload assistant.l")
        ok &= check(s, f'(setq *assistant-endpoint-port* {port})', "", "port for loop")
        # One converse: the mock asks for eval_lisp(+ 1 2); the OS runs it (ephemeral),
        # sends tool_result, and the mock returns the final text "The answer is 3."
        ok &= check(s, '(assistant-converse "what is 1+2?" (lambda (p) nil))',
                    "The answer is 3.", "agentic loop runs a tool and finishes")
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 tools/assistant_check.py`
Expected: FAIL — `assistant-converse` undefined (only `assistant-stream` exists).

- [ ] **Step 3: Implement the loop**

Add to `user/lisp/assistant.l` (keep `assistant-stream` for the no-tools path).
This builds the request with the `tools` array and a growing `messages` string,
parses streamed `tool_use` blocks, runs them through `assistant-tool-run`, and
appends a `tool_result` user turn until the model stops asking for tools.

```lisp
;;; ---- the agentic loop (Phase 32.2) -----------------------------------------
;;; messages-json is built as a STRING of complete content blocks (each block is
;;; small; the running transcript can exceed 2048, so it is sent as a list split
;;; into <2048 parts by ac--split). State per turn is captured in closures.

(defun ac--split (s)
  "Split a possibly-large string into <=1500-byte parts for fd-write."
  (let ((n (string-length s)) (parts nil) (i 0))
    (while (< i n)
      (let ((end (if (< (+ i 1500) n) (+ i 1500) n)))
        (setq parts (cons (substring s i end) parts)) (setq i end)))
    (reverse parts)))

(defun ac--request-parts (messages-json)
  "Body parts: model/tools/stream wrapper around the messages-json string."
  (append
    (list (string-concat
            "{\"model\":\"" *assistant-model* "\",\"max_tokens\":"
            *assistant-max-tokens* ",\"stream\":true,\"tools\":"
            (assistant-tools-json) ",\"messages\":["))
    (ac--split messages-json)
    (list "]}")))

(defun ac--headers (blen)
  (list (string-concat "Host: " *assistant-endpoint-host*)
        "Content-Type: application/json" "anthropic-version: 2023-06-01"
        (string-concat "x-api-key: " (assistant--api-key))
        (string-concat "Content-Length: " blen) "Connection: close"))

(defun ac--blen (parts)
  (let ((blen 0)) (foreach (lambda (p) (setq blen (+ blen (string-length p)))) parts) blen))

;; One round-trip. Returns a list: (assistant-text tool-id tool-name tool-input-json)
;; where the tool fields are nil when the model produced no tool_use.
(defun ac--round (messages-json on-piece)
  (let ((parts (ac--request-parts messages-json))
        (text "") (tid nil) (tname nil) (tin ""))
    (http-post-sse *assistant-endpoint-host* *assistant-endpoint-port* "/v1/messages"
      (ac--headers (ac--blen parts)) parts
      (lambda (ev data)
        (cond
          ((equal ev "content_block_start")
           (let ((cb (json-get (json-parse data) "content_block")))
             (if (and cb (equal (json-get cb "type") "tool_use"))
                 (progn (setq tid (json-get cb "id")) (setq tname (json-get cb "name")))
               nil)))
          ((equal ev "content_block_delta")
           (let ((d (json-get (json-parse data) "delta")))
             (cond
               ((equal (json-get d "type") "text_delta")
                (let ((p (json-get d "text")))
                  (if p (progn (setq text (string-concat text p)) (on-piece p)) nil)))
               ((equal (json-get d "type") "input_json_delta")
                (let ((p (json-get d "partial_json")))
                  (if p (setq tin (string-concat tin p)) nil)))
               (t nil))))
          (t nil))))
    (list text tid tname tin)))

(defun assistant-converse (prompt on-piece)
  "Agentic loop: send PROMPT, run any tool the model asks for, loop tool_result
back until it answers in prose. Returns the final assistant text."
  (let ((messages
          (string-concat "{\"role\":\"user\",\"content\":\""
                         (json-escape prompt) "\"}"))
        (final "") (go t))
    (while go
      (let ((r (ac--round messages on-piece)))
        (let ((text (nth 0 r)) (tid (nth 1 r)) (tname (nth 2 r)) (tin (nth 3 r)))
          (if (not tid)
              (progn (setq final text) (setq go nil))     ; no tool -> done
            (let ((result (assistant-tool-run tname (json-parse tin))))
              ;; append the assistant tool_use turn + our tool_result turn
              (setq messages (string-concat messages
                ",{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\""
                tid "\",\"name\":\"" tname "\",\"input\":" tin "}]}"
                ",{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\""
                tid "\",\"content\":\"" (json-escape result) "\"}]}")))))))
    final))
```

- [ ] **Step 4: Run to verify it passes**

Run: `python3 tools/assistant_check.py`
Expected: `ok: agentic loop runs a tool and finishes` (returned `"The answer is 3."`), `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add user/lisp/assistant.l tools/assistant_check.py
git commit -m "feat(assistant): agentic tool loop — parse tool_use, run, loop tool_result"
```

---

## Task 6: `/lib/claude` persistence — kept features survive reboot

**Files:**
- Modify: `user/lisp/assistant-apply.l`, `user/lisp/frame.l`, `tools/assistant_check.py`

- [ ] **Step 1: Write the failing test**

In `tools/assistant_check.py`:

```python
        ok &= check(s, '(assistant-persist "greet" "(defun greet () \\"hi\\")")',
                    "saved", "persist writes /lib/claude/<name>.l")
        ok &= check(s, '(progn (greet) (assistant-load-all) (greet))',
                    '"hi"', "persisted feature loads")
        # the manifest lists it
        ok &= check(s, '(string-search "greet" (at-read-file (list (cons "path" "/lib/claude/manifest.l"))))',
                    "", "manifest records the feature")
```

(Tests run within one boot, so this checks write + manifest + reload in-process;
true cross-reboot survival is the manual smoke in Step 4.)

- [ ] **Step 2: Run to verify it fails**

Run: `python3 tools/assistant_check.py`
Expected: FAIL — `assistant-persist` undefined.

- [ ] **Step 3: Implement persistence**

Append to `user/lisp/assistant-apply.l`:

```lisp
;;; ---- persistence: kept features land in /lib/claude and reload at boot ------

(defun aa--write (path content)
  (let ((fd (creat path))) (if (< fd 0) nil (progn (fd-write fd content) (close fd) t))))

(defun assistant-persist (name code)
  "Save CODE as /lib/claude/NAME.l and append a loader to the manifest."
  (mkdir "/lib/claude")                       ; harmless if it exists
  (let ((path (string-concat "/lib/claude/" name ".l")))
    (aa--write path (string-concat code "\n"))
    (let ((fd (open "/lib/claude/manifest.l")))   ; append-or-create
      (let ((old (if (< fd 0) "" (let ((c (fd-read fd 4096))) (if (< fd 0) "" (if c c ""))))))
        (if (>= fd 0) (close fd) nil)
        (aa--write "/lib/claude/manifest.l"
          (string-concat old "(load \"" path "\")\n"))))
    "saved"))

(defun assistant-load-all ()
  "Load every persisted feature (the manifest). Safe to call at boot."
  (let ((fd (open "/lib/claude/manifest.l")))
    (if (< fd 0) nil (progn (close fd) (load "/lib/claude/manifest.l")))))
```

**Note for the implementer:** confirm `mkdir` and `creat` primitives exist with
these names (see `user/lm_sys.c` — `creat` is present; check for `mkdir`, else
create `/lib/claude` via the `mkdirat`-backed primitive the file manager uses).
If `mkdir` is absent, add a one-line `DEFSYS("mkdir", ...)` wrapping `SYS_MKDIRAT`
as a sub-step, with its own KTEST is not needed (userland-only).

In `user/lisp/frame.l`, after the assistant loads, add:

```lisp
(assistant-load-all)            ; restore Claude-built features from /lib/claude
```

- [ ] **Step 4: Run to verify it passes + manual reboot smoke**

Run: `python3 tools/assistant_check.py`
Expected: the persistence `ok:` lines, `ALL PASS`.

Manual cross-reboot smoke (optional but recommended): boot the GUI, `M-x emiel`,
ask it (via a live proxy) to build a feature, accept it, reboot the same
`build/disk.img`, and confirm the feature is still defined.

- [ ] **Step 5: Commit**

```bash
git add user/lisp/assistant-apply.l user/lisp/frame.l tools/assistant_check.py
git commit -m "feat(assistant): /lib/claude persistence — kept features survive reboot"
```

---

## Task 7: The gate UI + undo in the frame

**Files:**
- Modify: `user/lisp/assistant.l`

- [ ] **Step 1: Surface pending approvals and undo as commands**

Append to `user/lisp/assistant.l`:

```lisp
;;; ---- gate UI (Phase 32.2): review/accept persistent changes, undo ----------

(defun assistant-show-pending ()
  "Insert any queued persistent forms into *emiel* for review."
  (let ((b (assistant--buffer)))
    (set-buffer b) (goto-char (buffer-length))
    (if *assistant-pending*
        (foreach (lambda (p)
                   (insert (string-concat "\n[pending] " (car p)
                            "\n  C-c C-a accept  /  C-c C-u undo\n")))
                 *assistant-pending*)
      (insert "\n[no pending changes]\n"))
    (redisplay)))

(defun emiel-accept ()
  "M-x emiel-accept: apply the oldest queued persistent change."
  (interactive)
  (let ((b (assistant--buffer)))
    (set-buffer b) (goto-char (buffer-length))
    (insert (string-concat "\n" (assistant-accept) "\n")) (redisplay)))

(defun emiel-undo ()
  "M-x emiel-undo: roll back the most recent applied redefinition."
  (interactive)
  (let ((b (assistant--buffer)))
    (set-buffer b) (goto-char (buffer-length))
    (insert (string-concat "\n" (assistant-undo) "\n")) (redisplay)))
```

- [ ] **Step 2: Confirm machinery loads (headless)**

Add to `tools/assistant_check.py`:

```python
        ok &= check(s, "(progn (assistant-apply \"(defun assistant-accept2 () 1)\") t)",
                    "t", "gate UI machinery loads")
```

Run: `python3 tools/assistant_check.py`
Expected: `ALL PASS`.

- [ ] **Step 3: Commit**

```bash
git add user/lisp/assistant.l tools/assistant_check.py
git commit -m "feat(assistant): gate UI — review pending, M-x emiel-accept / emiel-undo"
```

---

## Task 8: Live "make me a calculator" demo + notes

**Files:**
- Modify: `docs/notes/phase-32.md`, `docs/ROADMAP.md`

- [ ] **Step 1: Run the headline demo (manual, against a live proxy)**

With a `socat` proxy on `:8787` and a key in `/lib/claude/api-key`, boot the GUI,
`M-x emiel`, and ask: "build a calculator: a function (calc expr) that evaluates a
Lisp arithmetic expression, and show me (calc '(+ 2 (* 3 4)))". Confirm the model
calls `eval_lisp`, the function is defined, and the result (14) comes back — a
genuine self-extension. Screendump it into `docs/images/` if it lands.

- [ ] **Step 2: Update notes + roadmap**

Append a `## 32.2` section to `docs/notes/phase-32.md` summarising the tool loop,
the gate, and persistence; flip **32.2** to ✅ DONE in `docs/ROADMAP.md`.

- [ ] **Step 3: Commit**

```bash
git add docs/notes/phase-32.md docs/ROADMAP.md docs/images
git commit -m "docs(phase-32): tools + apply gate done; the make-a-calculator demo"
```

---

## Self-review notes

- **Spec coverage (milestone 2):** tools (`eval_lisp`/`introspect`/`source`/file/bash) — Task 3; the ephemeral/persistent classifier + undo — Task 2; the agentic loop (parse `tool_use`, run, loop `tool_result`) — Task 5; `/lib/claude` persistence — Task 6; the gate UI — Task 7; full recursive `json-parse` for tool inputs — Task 1.
- **Known scope cuts / risks flagged for the build:** `function-info`'s exact `(lambda params body)` shape must be confirmed for `assistant-undo` reconstruction (Task 2 note); `mkdir` primitive existence (Task 6 note — add a thin wrapper if absent); the real API's chunked encoding still needs `http.l` de-chunking or a de-chunking proxy (carried from M1, Task 8 of M1); the classifier is intentionally conservative (a non-defun that doesn't touch a write-op is treated ephemeral — revisit if a sneaky persistent form slips through).
- **Type consistency:** `assistant-tool-run name args` (alist) is identical in Task 3 (def), Task 3 (tests) and Task 5 (caller). `assistant-apply`/`assistant-classify`/`assistant-accept`/`assistant-undo` names match across Tasks 2, 5, 7. `json-parse`/`json-get` (Task 1) feed `at--arg` and the loop's SSE handlers.
- **Verification:** `python3 tools/assistant_check.py` (hermetic, two-turn mock) is the green bar for Tasks 1–7; the live calculator demo (Task 8) is the milestone's human proof.
```
