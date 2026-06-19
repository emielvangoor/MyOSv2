# Phase 32 notes — Claude in the image

## 32.1 — proof of life (proxy)  ✅ DONE

The assistant speaks **plaintext HTTP to a host-side TLS proxy** that forwards to
`https://api.anthropic.com`. From the guest, the host is `10.0.2.2` (QEMU
user-net redirects the gateway to the host loopback — verified by the
`connect to mock 10.0.2.2` smoke in `tools/assistant_check.py`).

### What shipped

- `src/netstack.c` — `net_resolve` accepts a dotted-quad IP literal (so
  `(connect fd "10.0.2.2" port)` reaches the proxy without DNS). KTEST
  `net: resolve dotted-quad literal`.
- `user/lisp/json.l` — `json-escape` (request bodies) + `json-string-value`
  (pull a decoded string field out of an SSE `data:` frame). Full recursive
  `json-parse` is deferred to 32.2.
- `user/lisp/http.l` — `http-post-sse`: one POST, stream the `text/event-stream`
  reply, call back per SSE frame. No chunked decoding (the proxy/mock present an
  unchunked stream, `Connection: close`).
- `user/lisp/assistant.l` — `assistant-stream` builds the Messages request and
  assembles the streamed reply; `M-x emiel` streams a greeting into the `*emiel*`
  buffer. `*assistant-name*` (default `"emiel"`) names the buffer + reply label.
- Verified: `python3 tools/assistant_check.py` (hermetic, tokenless — drives a
  mock SSE endpoint, `tools/mock_anthropic.py`), and a live-frame screendump of
  `M-x emiel` rendering the mock reply in `*emiel*`.

### Talking to the real API (live use)

Run a TLS proxy on the host (the guest sees it at `10.0.2.2`):

    socat TCP-LISTEN:8787,reuseaddr,fork OPENSSL:api.anthropic.com:443

Put your key in `/lib/claude/api-key` (one line), then in the OS:

    (setq *assistant-endpoint-host* "10.0.2.2")
    (setq *assistant-endpoint-port* 8787)   ; the default already
    M-x emiel

### Caveats (accepted for a hobby OS; see the spec)

- The API key is plaintext at `/lib/claude/api-key` (single-user, no perms yet).
- `socat OPENSSL` passes the API's **chunked** encoding through verbatim, while
  `http.l` reads an unchunked stream. For the *real* endpoint either add chunked
  decoding to `http.l` (a 32.x follow-up) or front it with a de-chunking proxy.
  The hermetic mock is already unchunked, so tests are unaffected.
- `json-escape` / `json-string-value` inherit the 2048-byte `string-concat` cap
  (fine for short M1 prompts; M2 streams).

## 32.2 — tools + the apply gate  ✅ DONE

The proof-of-life stream became a real agent: Claude calls tools, the OS runs
them, and generated Lisp installs itself into the live image. Plan:
`docs/superpowers/plans/2026-06-19-assistant-milestone-2.md`.

### What shipped

- `json.l` — a real recursive `json-parse` + `json-get` (decode tool inputs),
  alongside the M1 SSE text extractor.
- `assistant-apply.l` — **the hybrid gate**: `assistant-classify` (a fresh defun
  / plain computation is *ephemeral* → auto-eval; redefining an existing
  function or any file/registry write is *persistent* → queued), `assistant-apply`,
  `assistant-accept`, and `assistant-undo` (per-symbol snapshot rollback). Plus
  `/lib/claude` persistence (`assistant-persist` + a boot manifest loaded by
  `assistant-load-all`).
- `assistant-tools.l` — the tools: `eval_lisp` (gated), `introspect_image`
  (symbols + kind), `function_source` (living source), `read_file`, `write_file`
  (gated), `run_bash` (busybox) + the JSON tool schemas.
- `assistant.l` — `assistant-converse`: the agentic loop. Parses streamed
  `tool_use` blocks (id/name + `input_json_delta` fragments), runs the tool,
  appends `tool_use` + `tool_result` turns, loops until the model answers in
  prose. `M-x emiel` now drives this loop; `M-x emiel-accept` / `emiel-undo`
  manage the gate.
- New kernel-adjacent: a `mkdir` Lisp primitive (`SYS_MKDIRAT` wrapper) so
  `/lib/claude` can be created.
- Verified: `tools/assistant_check.py` against the two-turn mock (tool_use →
  run → tool_result → final), **and** a live-frame screendump of the loop
  (`docs/images/phase-32-agent.png`): `M-x emiel` → `eval_lisp(+ 1 2)` →
  "The answer is 3.".

### Known limits (M2)

- ~~Tool results / `read_file` / `introspect` cap at ~2048 bytes.~~ **Fixed:**
  `string-concat` is now dynamic (two-pass, exact-size alloc in `lm_core.c`;
  KTEST `lm: strings` concatenates past 2560 bytes), so file reads, the
  transcript, JSON escaping and tool results no longer truncate.
- The classifier is deliberately conservative; a novel persistent form that
  doesn't name a known write-op could slip through as ephemeral — revisit the
  rule as real usage surfaces cases.
- The real API's chunked encoding still needs `http.l` de-chunking or a
  de-chunking proxy (carried from M1) before going fully proxy-free.

### Next (32.3)

Vendor BearSSL + a `getrandom` syscall, embed trust anchors, and flip
`*assistant-endpoint*` to `https://api.anthropic.com` — drop the proxy.
