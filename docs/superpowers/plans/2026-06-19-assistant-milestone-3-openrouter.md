# Assistant Milestone 3 — OpenRouter provider Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Run the agent through **OpenRouter** — your own OpenRouter key, OpenRouter bills you, and you reach Claude (and any other model) through one endpoint. Reuses everything from M2 (tools, gate, loop); swaps the provider's wire format.

**Architecture:** OpenRouter speaks the **OpenAI chat-completions** format, which differs from Anthropic's Messages API in three ways: request shape (`messages` + OpenAI `tools`), SSE deltas (`choices[].delta.content` / `.tool_calls`), and auth (`Authorization: Bearer`). Real endpoints also use **HTTP chunked transfer-encoding**, which `http.l` must now decode. We add (1) chunked decoding to `http.l`, (2) an OpenRouter request/parse path, and (3) a `*assistant-provider*` switch. Still proxy-fronted for TLS (`socat OPENSSL:openrouter.ai:443`); BearSSL is a later milestone.

**Tech Stack:** the MyOSv2 Lisp dialect (M1/M2 `http.l`/`json.l`/`assistant*.l`), Python (mock OpenRouter endpoint + harness).

## Dialect gotchas (carried)

CRLF via `(string-from-char 13)` (no `\r`); top-level `setq` (no `defvar`);
`string-search` is `(needle haystack)`; `equal` for strings; nest `let`.
`string-concat` is now dynamic (M2 fix) — large bodies/transcripts are fine.

## File structure

- **Modify** `user/lisp/http.l` — decode `Transfer-Encoding: chunked` before SSE framing (provider-agnostic; the M1 unchunked path still works).
- **Create** `user/lisp/assistant-openrouter.l` — OpenAI-format request builder, SSE delta interpreter, OpenAI tool schemas, and the OpenRouter agentic round.
- **Modify** `user/lisp/assistant.l` — `*assistant-provider*` switch; `assistant-converse` dispatches to the Anthropic or OpenRouter round; auth header is pluggable.
- **Modify** `Makefile:38` — add `assistant-openrouter`.
- **Modify** `tools/mock_anthropic.py` — an OpenRouter mode (chunked, OpenAI SSE, two-turn tool_calls), selected by the request path `/api/v1/chat/completions`.
- **Modify** `tools/assistant_check.py` — chunked + OpenRouter loop assertions.

---

## Task 1: `http.l` — decode chunked transfer-encoding

**Files:** Modify `user/lisp/http.l`; Modify `tools/mock_anthropic.py`, `tools/assistant_check.py`.

Add a chunked decoder and restructure `http--read-stream` to: read raw → strip headers (detect chunked) → de-chunk into a body stream → frame SSE lines from the body.

Key new helpers:

```lisp
(defun http--is-chunked (hdr) (if (string-search "chunked" hdr) t nil))

(defun http--hex (s)               ; leading hex digits -> integer (ignores ;ext)
  (let ((n (string-length s)) (i 0) (val 0) (go t))
    (while (and go (< i n))
      (let ((c (string-ref s i)))
        (cond ((and (>= c 48) (<= c 57)) (setq val (+ (* val 16) (- c 48))))
              ((and (>= c 97) (<= c 102)) (setq val (+ (* val 16) (- c 87))))
              ((and (>= c 65) (<= c 70)) (setq val (+ (* val 16) (- c 55))))
              (t (setq go nil))))
      (if go (setq i (+ i 1)) nil))
    val))

;; state: -1 need-size, -2 need-trailing-CRLF, -3 done, >=1 bytes left in chunk
(defun http--dechunk (raw state)   ; -> (decoded new-raw new-state)
  (let ((out "") (go t))
    (while go
      (cond
        ((= state -1)
         (let ((nl (string-search "\n" raw)))
           (if (not nl) (setq go nil)
             (let ((sz (http--hex (http--strip-cr (substring raw 0 nl)))))
               (setq raw (substring raw (+ nl 1) (string-length raw)))
               (if (= sz 0) (progn (setq state -3) (setq go nil)) (setq state sz))))))
        ((= state -2)
         (if (< (string-length raw) 2) (setq go nil)
           (progn (setq raw (substring raw 2 (string-length raw))) (setq state -1))))
        ((> state 0)
         (if (= (string-length raw) 0) (setq go nil)
           (let ((take (if (< (string-length raw) state) (string-length raw) state)))
             (setq out (string-concat out (substring raw 0 take)))
             (setq raw (substring raw take (string-length raw)))
             (setq state (- state take))
             (if (= state 0) (setq state -2) nil))))
        (t (setq go nil))))
    (list out raw state)))
```

`http--read-stream` becomes (decoded-body framing identical to before):

```lisp
(defun http--read-stream (fd on-event)
  (let ((raw "") (hdr-done nil) (chunked nil) (cstate -1)
        (body "") (ev "message") (data "") (go t))
    (while go
      (let ((chunk (fd-read fd 4096)))
        (if (not chunk) (setq go nil)
          (progn
            (setq raw (string-concat raw chunk))
            (if (not hdr-done)
                (let ((b (string-search (string-concat http-crlf http-crlf) raw)))
                  (if b (progn
                          (setq chunked (http--is-chunked (substring raw 0 b)))
                          (setq raw (substring raw (+ b 4) (string-length raw)))
                          (setq hdr-done t))
                    nil))
              nil)
            (if hdr-done
                (progn
                  (if chunked
                      (let ((r (http--dechunk raw cstate)))
                        (setq body (string-concat body (nth 0 r)))
                        (setq raw (nth 1 r)) (setq cstate (nth 2 r)))
                    (progn (setq body (string-concat body raw)) (setq raw "")))
                  (let ((nl (string-search "\n" body)))
                    (while nl
                      (let ((line (http--strip-cr (substring body 0 nl))))
                        (setq body (substring body (+ nl 1) (string-length body)))
                        (cond
                          ((= (string-length line) 0)
                           (http--dispatch ev data on-event)
                           (setq ev "message") (setq data ""))
                          ((and (>= (string-length line) 6)
                                (equal (substring line 0 6) "event:"))
                           (setq ev (http--trim-leading-space (substring line 6 (string-length line)))))
                          ((and (>= (string-length line) 5)
                                (equal (substring line 0 5) "data:"))
                           (setq data (http--trim-leading-space (substring line 5 (string-length line)))))
                          (t nil)))
                      (setq nl (string-search "\n" body)))))
              nil)))))
    (http--dispatch ev data on-event)))
```

Mock: add a `/chunked` path that returns a chunked Anthropic-style SSE of "Hello from the mock." (reuse the existing pieces, but framed as chunks). Test: `http-post-sse` to `/chunked` assembles the same text. Plus confirm the existing unchunked path still passes.

- [ ] Steps: write the failing chunked assertion → run (fail) → implement the decoder + read-stream → run (pass) → commit `feat(assistant): http chunked transfer decoding`.

---

## Task 2: `assistant-openrouter.l` — OpenAI-format round + tools + request

**Files:** Create `user/lisp/assistant-openrouter.l`; Modify `Makefile:38`, `tools/assistant_check.py`.

```lisp
;;; assistant-openrouter.l -- the OpenRouter (OpenAI chat-completions) provider.

(defun or-tools-json ()
  "Wrap each tool as an OpenAI function tool."
  (string-concat
    "[{\"type\":\"function\",\"function\":{\"name\":\"eval_lisp\",\"description\":\"Evaluate a Lisp form in the live OS image. Ephemeral forms run immediately; persistent ones are queued for approval.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"}},\"required\":[\"code\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"introspect_image\",\"description\":\"List interned symbols containing a filter.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\"}}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read a file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"run_bash\",\"description\":\"Run a shell command via busybox.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}}}]"))

(defun or--request-parts (messages-json)
  (append
    (list (string-concat
            "{\"model\":\"" *assistant-model* "\",\"stream\":true,\"tools\":"
            (or-tools-json) ",\"messages\":["))
    (ac--split messages-json)
    (list "]}")))

;; One OpenRouter round -> (text tool-id tool-name tool-args-json)
(defun or--round (messages-json on-piece)
  (let ((parts (or--request-parts messages-json))
        (text "") (tid nil) (tname nil) (tin ""))
    (http-post-sse *assistant-endpoint-host* *assistant-endpoint-port*
      "/api/v1/chat/completions" (ac--headers (ac--blen parts)) parts
      (lambda (ev data)
        (if (equal data "[DONE]") nil
          (let ((choice (car (json-get (json-parse data) "choices"))))
            (if choice
              (let ((delta (json-get choice "delta")))
                (let ((c (json-get delta "content")))
                  (if (and c (> (string-length c) 0))
                      (progn (setq text (string-concat text c)) (on-piece c)) nil))
                (let ((tcs (json-get delta "tool_calls")))
                  (if tcs
                      (let ((tc (car tcs)))
                        (let ((idv (json-get tc "id"))) (if idv (setq tid idv) nil))
                        (let ((fn (json-get tc "function")))
                          (let ((nm (json-get fn "name"))) (if nm (setq tname nm) nil))
                          (let ((ag (json-get fn "arguments")))
                            (if ag (setq tin (string-concat tin ag)) nil))))
                    nil)))
              nil)))))
    (list text tid tname tin)))

(defun or--append-tool (messages tid tname tin result)
  "Append the OpenAI assistant tool_call turn + the tool result turn."
  (string-concat messages
    ",{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"" tid
    "\",\"type\":\"function\",\"function\":{\"name\":\"" tname
    "\",\"arguments\":" (json-encode-string tin) "}}]}"
    ",{\"role\":\"tool\",\"tool_call_id\":\"" tid
    "\",\"content\":\"" (json-escape result) "\"}"))
```

(`tin` is already a JSON string fragment; `json-encode-string` wraps it as a JSON
string value — add `(defun json-encode-string (s) (string-concat "\"" (json-escape s) "\""))`
to `json.l` if absent.)

- [ ] Steps: add `assistant-openrouter` to `LISP_FILES`; write the failing tool-schema/round-loads assertion → implement → pass → commit.

---

## Task 3: provider switch in `assistant.l`

```lisp
(setq *assistant-provider* 'openrouter)   ; 'anthropic | 'openrouter

(defun assistant--round (messages on-piece)
  (if (eq *assistant-provider* 'openrouter)
      (or--round messages on-piece) (ac--round messages on-piece)))

(defun assistant--append-tool (messages tid tname tin result)
  (if (eq *assistant-provider* 'openrouter)
      (or--append-tool messages tid tname tin result)
      (string-concat messages   ; the M2 Anthropic shape
        ",{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"" tid
        "\",\"name\":\"" tname "\",\"input\":" tin "}]}"
        ",{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"" tid
        "\",\"content\":\"" (json-escape result) "\"}]}")))
```

`assistant-converse` calls `assistant--round` / `assistant--append-tool` instead
of the hard-coded `ac--*`. Auth header becomes pluggable in `ac--headers`:
Bearer for OpenRouter, `x-api-key` for Anthropic.

- [ ] Steps: refactor `assistant-converse`; run the existing Anthropic loop test (no regression) → commit.

---

## Task 4: mock OpenRouter mode + the end-to-end loop

**Files:** Modify `tools/mock_anthropic.py`, `tools/assistant_check.py`.

Mock: when the request path is `/api/v1/chat/completions`, respond **chunked**
with OpenAI SSE. First turn (no `"role":"tool"` in body) → a `tool_calls` delta
for `eval_lisp {"code":"(+ 1 2)"}` then `finish_reason:"tool_calls"`; second turn
(`"role":"tool"` present) → content deltas "The answer is 3." then `[DONE]`.

Test: set `*assistant-provider*` to `'openrouter`, point host/port at the mock,
`(assistant-converse "what is 1+2?" ...)` → `"The answer is 3."`.

- [ ] Steps: extend mock → assertion → run (pass) → commit.

---

## Task 5: wire the default + docs

- `assistant-ask` already calls `assistant-converse`; default `*assistant-provider*`
  to `'openrouter`, default `*assistant-endpoint-port*` to the proxy, default
  `*assistant-model*` to a valid OpenRouter slug (e.g. `anthropic/claude-3.5-sonnet`).
- `docs/notes/phase-32.md` §32.3: `socat TCP-LISTEN:8787,reuseaddr,fork OPENSSL:openrouter.ai:443`,
  key in `/lib/claude/api-key`, `(setq *assistant-model* "...")`, `M-x emiel`.
- Flip roadmap (a new **32.3 — OpenRouter provider**; renumber the BearSSL TLS work to 32.4).

- [ ] Commit `docs(phase-32): OpenRouter provider`.

## Self-review

- Coverage: chunked (T1), OpenAI request/SSE/tools (T2), provider switch (T3), end-to-end loop (T4), defaults+docs (T5).
- Risks: case-sensitive `"chunked"` match (value is conventionally lowercase — fine); OpenRouter model slugs change (user-set); still proxy-fronted (BearSSL = 32.4); the real chunked stream interleaves SSE frames across chunk boundaries — the decoder buffers partial chunks, so framing is unaffected.
- Verify: `tools/assistant_check.py` (chunked + OpenRouter two-turn mock), hermetic.
