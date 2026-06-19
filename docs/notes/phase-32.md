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

### Next (32.2)

Tools (`introspect-image`, `eval-lisp`, file + bash) and the hybrid apply gate
(ephemeral auto-eval, persistent preview+accept) with undo and `/lib/claude`
persistence — where "make me a calculator" first works.
