# Claude in the image — a native AI agent that rewrites the OS (`M-x emiel`) — Design

**Date:** 2026-06-19
**Goal:** Make Claude a *co-resident of the live Lisp image*: you describe a
feature in plain language and the OS grows it, because the agent generates Lisp
and the running image `eval`s it. Need a calculator? It didn't exist a second
ago and now it does — no compile, no link, no reboot. The assistant's name lives
in a Lisp variable (default `"emiel"`) and is overridable from inside the image
like everything else.

## Why this belongs on *this* OS (and almost nowhere else)

On a Unix box "add a feature" means edit → compile → link → restart. A process
cannot `eval` itself. MyOSv2's userland **is** a live, redefinable Lisp image
(reader, evaluator, macros, hot redefinition, GC), so a generated S-expression
becomes behaviour the instant it is read. Three properties make the agent loop
natural here:

1. **`eval` into the live image** — generated code is installed by evaluating it,
   not by spawning a process. The OS mutates itself in place.
2. **Living source introspection** — the image can already hand back the *actual
   lambda* a function runs (`C-h f`), so the agent can read how the OS works
   before writing code that fits it.
3. **Cheap rollback** — a symbol's old value is one cell; snapshotting what a
   change redefines (and restoring it) is trivial. C programs can't be un-run; a
   Lisp image can.

This is not "Claude Code ported to MyOSv2." It is the thing Claude Code can't be:
**the OS as the agent.**

## Decision: a native Lisp agent (Path B), hybrid-by-scope, name-as-variable

We do **not** attempt to run the Node-based Claude Code binary (that needs real
threads + futex, JIT-executable mmap, libuv's epoll/eventfd/timerfd backend, a
full C++ runtime and BoringSSL — a multi-month research effort; see the
"Rejected" section). Instead the *capability* — an HTTPS client to the Anthropic
Messages API plus a tool-execution loop — is reimplemented as a Lisp program
native to the image. The heavy lifting (the model) stays in the cloud; what runs
locally is a streaming request/response loop and some tool plumbing, which the
image is already shaped to host.

### Settled choices

- **Proxy-first, TLS-later.** The first working agent talks to the real API
  *through a host-side TLS-terminating proxy* (plaintext HTTP from the OS over
  QEMU user-net `10.0.2.2` → HTTPS to `api.anthropic.com`). Zero crypto in the
  OS on day one; TLS becomes an upgrade that removes the proxy, never a
  prerequisite for proof-of-life.
- **Hybrid-by-scope safety.** Generated forms are *classified*: **ephemeral**
  ones (define a fresh symbol, open a buffer, draw) auto-eval instantly;
  **persistent** ones (write a file, redefine an *existing* bound symbol,
  register a permanent command/keybinding) pause for a one-key preview+accept.
  The calculator-magic stays instant; the OS can't be silently rewritten.
- **Name lives in Lisp.** `*assistant-name*` (default `"emiel"`) controls the
  `M-x` command alias, the buffer name (`*emiel*`) and the reply label
  (`emiel:`). The machinery underneath is name-agnostic, so overriding is pure
  renaming, never rewiring.
- **No big-string accumulation.** `string-concat` truncates at ~2048 bytes (a
  fixed C buffer — see [[vt100-terminal]] lessons), so request bodies and
  responses are **streamed**: the JSON request is written directly to the socket
  through a small fd-writer, and the SSE response is parsed incrementally from
  small reads. We never build the whole conversation as one Lisp string.
- **Keep the existing socket stack.** The agent uses the `socket`/`connect`
  Lisp bindings and `resolve` (DNS) that already exist; no new transport.

## Architecture (bottom-up)

```
Layer 4  UI + persona     M-x <*assistant-name*> buffer, minibuffer "ask",
                          reply streaming into *emiel*, /lib/claude manifest
Layer 3  Apply gate       classify ephemeral|persistent, preview+accept,
                          per-symbol snapshot + undo, persist kept features
Layer 2  Tools            introspect-image, eval-lisp, read/write-file, run-bash
Layer 1  Agent loop       assistant-converse: stream → tool_use → run → tool_result
Layer 0  Substrate        HTTP/1.1 + JSON (streamed) over sockets; SSE parser;
                          [later] BearSSL TLS + getrandom; [now] host TLS proxy
```

### Layer 0 — Substrate: HTTP/JSON over sockets, streamed

A small Lisp module (`user/lisp/http.l`) provides:
- **`http-post`** — open a socket to `(resolve host)`:port, write an HTTP/1.1
  request **directly to the fd** (request line, headers, then the body produced
  by a *body-writer thunk* so the JSON is never materialised as one string),
  then return the connected fd for streamed reading.
- **A streaming reader** — read the status line + headers, then hand the body to
  a line/▸event consumer. The Messages API streams **Server-Sent Events**
  (`text/event-stream`): `event: <type>\n` + `data: <json>\n\n` frames. We parse
  frames incrementally (same small-chunk discipline the vterm poll uses) and
  dispatch on event type.
- **JSON** (`user/lisp/json.l`) — a `json-write` that emits to an fd/writer (not
  a string) for requests, and a `json-read` that parses a complete value from a
  string for the small JSON payloads inside each SSE `data:` frame (those are
  individually small — a text delta, a partial tool-input chunk — so per-frame
  string parsing is safe).

Endpoint and auth:
- `POST {*assistant-endpoint*}/v1/messages` — `*assistant-endpoint*` defaults to
  the proxy (`http://10.0.2.2:8787`), later flips to `https://api.anthropic.com`.
- Headers: `x-api-key: <key>`, `anthropic-version: 2023-06-01`,
  `content-type: application/json`. The key is read from `/lib/claude/api-key`
  (one line); **caveat:** plaintext on disk on a single-user hobby OS — noted,
  not solved here.
- Body: `{ model, max_tokens, system, messages, tools, stream: true }`. Model
  default in `*assistant-model*`.

### Layer 1 — The agent loop (`user/lisp/assistant.l`)

`assistant-converse` is the harness, ~a page of Lisp, mirroring the standard
tool-use loop:

1. Append the user's turn to the running `messages` list (per-`*emiel*`-buffer
   state).
2. `http-post` the request; consume the SSE stream, accumulating: assistant text
   (streamed live into the buffer, labelled `*assistant-name*:`) and any
   `tool_use` blocks (id, name, and input assembled from `input_json_delta`
   fragments).
3. On `message_stop`: if there were `tool_use` blocks, **run each tool**, append
   one `tool_result` per call, and **loop** (step 2) with no new user input —
   exactly the agentic cycle. If there were none, the turn is done; await input.

SSE event handling: `content_block_start` (open a text or tool_use block),
`content_block_delta` (`text_delta` → append to buffer; `input_json_delta` →
accumulate tool input), `content_block_stop`, `message_delta` (stop_reason),
`message_stop`. Errors (`event: error`, or an HTTP non-200) surface in the
buffer and abort the turn cleanly — one bad turn never wedges the loop (same
fresh-recovery-per-tick spirit as `frame-tick`).

### Layer 2 — Tools (what makes generated code *fit the image*)

Declared in the request's `tools` array; dispatched by name in `assistant.l`:

- **`introspect-image`** — list bound symbols (optionally filtered), and fetch a
  function's *living source* via the existing describe-function path. This is the
  agent reading your OS before writing for it.
- **`eval-lisp`** — read+eval a form in the live image, returning the printed
  result or the error. The core move. **Routed through the Layer-3 gate**, so an
  ephemeral form runs immediately while a persistent one returns "awaiting your
  approval" until you accept.
- **`read-file` / `write-file`** — over the existing file syscalls; `write-file`
  is always persistent (gated).
- **`run-bash`** — fork/exec busybox for shell-outs (e.g. `grep`, `ls`),
  capturing output. The same shape as the frame's job runner.

Tools return structured `tool_result` content (text, or `is_error: true`).

### Layer 3 — The apply gate, undo, persistence (`user/lisp/assistant-apply.l`)

**Classifier** — given a form to install, decide ephemeral vs persistent:
- *Persistent* if it: redefines an already-bound symbol (`defun`/`defvar`/`setq`
  on an existing global), registers a permanent command or keybinding, or calls
  a file-writing primitive (`write-file`/`creat`/`save-buffer`).
- *Ephemeral* otherwise (defines a fresh symbol, opens/draws a buffer, computes).
- Implementation: walk the top-level form's head and its bound name; check the
  symbol table for prior binding; scan for write primitives. Conservative —
  unknown ⇒ treat as persistent (safe default).

**Ephemeral path:** `eval` immediately; the result streams back to the agent and
the buffer. "Make me a calculator" → a fresh `calc` defun + a window → just
happens.

**Persistent path:** render the form (pretty-printed, faces) into a preview pane
with a one-line summary of *what it touches* ("redefines `frame-tick`",
"writes `/lib/claude/units.l`"); `y` applies, `n` declines, and the decision is
returned to the agent as the tool result so it can react.

**Undo:** before any apply, snapshot the *old values* of every symbol the form
redefines (and note files it will create). `assistant-undo` (and `C-x z`)
restores the previous bindings / removes created files. Per-change snapshots,
not a whole-image dump — cheap and sufficient.

**Persistence ("the OS remembers"):** a feature you keep is written to
`/lib/claude/<name>.l` and registered in `/lib/claude/manifest.l`, which the boot
sequence loads. The calculator you built yesterday is still there after a reboot
— the OS literally accretes the features you ask for.

### Layer 4 — UI and persona

- **`*assistant-name*`** (default `"emiel"`) + `set-assistant-name` re-aliases
  the `M-x` command, the buffer name and the reply label live:
  ```lisp
  (defvar *assistant-name* "emiel")
  (defun set-assistant-name (n)
    (setq *assistant-name* n)
    (register-command n 'assistant-converse))   ; M-x emiel → M-x <n>
  ```
- **Conversation buffer** — `M-x emiel` opens/visits `*emiel*` in
  `assistant-mode` (a major mode over [[the mode system|major-modes]]): your
  input at the bottom, RET sends, replies stream in labelled, tool calls and
  gate previews render inline.
- **Minibuffer ask** — a key (e.g. `M-RET`) grabs a one-line request from
  anywhere ("make a calculator") and runs one turn without leaving your current
  window — the "the OS understands me everywhere" feel.

## Milestones (decomposed — too big for one spec/plan)

Each is its own spec→plan→build→notes cycle, gated on `make test`, and leaves the
OS working.

1. **Proof of life** *(no kernel work)* — `http.l` + `json.l` + a minimal
   `assistant-converse` that streams a reply into `*emiel*`, via the host TLS
   proxy. Verified: a real Messages API round-trip renders in the buffer. **This
   spec's first plan.**
2. **Tools + the gate** — `introspect-image`, `eval-lisp`, file/bash tools, the
   ephemeral/persistent classifier, undo, `/lib/claude` persistence. Verified:
   "make me a calculator" creates and runs it; a `frame-tick` redefinition is
   gated and undoable.
3. **TLS** — vendor **BearSSL** (small, pure-C, no-malloc, bare-metal-friendly)
   over the existing sockets + a `getrandom` syscall to seed it + embedded trust
   anchors; flip `*assistant-endpoint*` to `https://api.anthropic.com`, drop the
   proxy. Verified: a direct HTTPS turn with no proxy running.
4. **Polish** — `assistant-mode` niceties, minibuffer-ask, boot manifest,
   richer tools (apply-patch-style edits, multi-file features).

## Verification

- **Milestone 1:** `tools/assistant_check.py` boots the OS with the host proxy
  pointed at a **mock** Messages endpoint (deterministic SSE script), runs one
  turn, and asserts the streamed text and `*emiel*` buffer content via the TCP
  REPL / screendump. (Mock endpoint keeps the test hermetic and tokenless.)
- **Milestone 2:** the same harness scripts a `tool_use(eval-lisp)` for a fresh
  `defun`, asserts the new symbol is bound and callable; scripts a redefinition
  of an existing symbol, asserts it is *gated* (not applied until accepted) and
  that `assistant-undo` restores the original.
- **Milestone 3:** `tools/assistant_tls_check.py` does a direct HTTPS handshake
  to the mock endpoint served over TLS (self-signed anchor embedded for the
  test), no proxy.
- KTESTs for any new kernel surface (`getrandom`, BearSSL glue) red→green.

## Security / safety considerations

- **Evaluating model output** is the core risk; the gate (review persistent
  changes), conservative classification (unknown ⇒ persistent), and per-change
  undo are the mitigations. There is no isolation beyond the single-user image —
  acceptable for a hobby OS, stated plainly.
- **API key** is plaintext at `/lib/claude/api-key`. Single-user, no users/perms
  yet (that's a "Later/advanced" roadmap item). Noted, not solved.
- **TLS entropy** on a deterministic VM is weak; `getrandom` seeds from timer
  jitter + whatever is available. Fine for a toy; a real CSPRNG/entropy source
  is out of scope. Noted.
- **Trust anchors** for direct HTTPS must be embedded; until milestone 3 the
  proxy holds all crypto.

## Rejected: running the actual Node Claude Code binary

Requires, on top of everything above: real threads (`CLONE_VM` is `-ENOSYS`, no
futex), JIT-executable memory (mmap is anon-only and hard-wired non-executable,
no `mprotect`), libuv's Linux event loop (`epoll`/`eventfd`/`timerfd` — none
exist; we have `poll`/`ppoll`), a full C++ runtime with unwinding, BoringSSL, and
enough Node-API fidelity that a ~50 MB bundled app survives startup. Multi-month
research effort and *still* not "the OS as the agent." Reimplementing the
capability in Lisp is both far more tractable and the whole point.
