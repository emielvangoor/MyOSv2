# Major modes for the Lisp machine — design

**Goal:** give the graphical Lisp machine a real Emacs-style **major-mode**
system, so a buffer's behavior (what a key does, what RET does, what the mode
line says) is decided by its mode rather than by scattered `(if (editing-p) …)`
conditionals. Deliver three first-class modes — `text-mode`, `repl-mode`, and
`lisp-interaction-mode` — a default `*scratch*` buffer, and an interactive
`C-x C-f`.

North-star feel: boot into `*scratch*` (lisp-interaction-mode) like Emacs; open
a file with `C-x C-f` into a plain text buffer; the mode line names the active
mode.

## Context: what exists today

`user/lisp/frame.l` (726 lines) is the graphical event loop + REPL. Relevant
current state:

- **No mode abstraction.** A buffer is a REPL iff its handle is in the
  `repl-bufs` list; `editing-p` is just "not a REPL." Mode-specific behavior is
  hardcoded as `(if (editing-p) …)` in `dispatch` and `describe-mode`.
- **Per-buffer state is already buffer-keyed alists**: `repl-starts`
  (handle → prompt position) and `file-of` (handle → path). Buffer-local
  variables are a natural generalization of this existing pattern.
- **find-file / save-buffer exist** but `find-file` is a *function called with a
  path from the REPL* that auto-splits a window; `C-x C-f` is **not** bound.
  `C-x C-s` → `save-buffer` is bound (in `cx-keymap`).
- **Keymaps are already data**: `global-keymap`, `cx-keymap`, `help-keymap` are
  lists of `(kind code command description)`; `run-key`/`key-lookup` read them;
  C-h b/k/m are generated from them.
- **The C renderer already draws a mode line** (`src/rd_core.c`, label
  `modeline:`): each window's last row shows `-- <buffer-name> -----` in face 1
  (inverse). It shows the buffer name, not the mode.
- **Boot**: `frame-setup` makes the first buffer a `*repl*` and prompts.
- The serial REPL (`lisp`, no `-frame`) does **not** load `frame.l`; only the
  frame (`lisp -frame`) does. So any logic that must be unit-tested over the
  plain serial REPL has to live in a file the serial REPL also loads
  (`bootstrap.l` / `system.l`).

## Architecture

### 1. Code layout (drives testability)

Split into a pure model and the frame wiring:

- **`user/lisp/modes.l` (new)** — the *pure* mode model: the mode registry,
  `define-mode` / `define-derived-mode`, derived-keymap lookup, and a generic
  buffer-local variable store. Plain list operations, no graphics primitives, so
  it loads in the **serial REPL** and is unit-testable by evaluating a form over
  serial and checking the printed result.
- **`frame.l` (rewrite the dispatch core + boot + C-x C-f)** — the *wiring*:
  route input through the active buffer's mode, interactive `C-x C-f`, set the
  mode-line text. Tested via the QMP-keyboard + screendump-OCR harness.

Load order becomes `bootstrap → system → modes → frame`. The build's lisp blob
loop and the runtime loader both gain `modes`.

### 2. The mode object

A mode is a symbol with a descriptor stored in a registry alist `*modes*`
(`mode-symbol → descriptor`). The descriptor is a list:

```
(parent  keymap  on-self-insert  on-return  setup-fn  doc  mode-line-name)
```

- **parent** — another mode-symbol, or nil for the root. Enables derived modes.
- **keymap** — a mode-local keymap (same `(kind code command description)`
  shape as `global-keymap`); may be nil.
- **on-self-insert** — a function symbol taking one arg (the char code), or nil
  to inherit. What a printable char does.
- **on-return** — a function symbol taking no args, or nil to inherit. What RET
  does.
- **setup-fn** — a function symbol taking no args, or nil. The "mode hook": run
  when a buffer *enters* the mode (prints the prompt, drops a banner, …).
- **doc** — a string for `C-h m`.
- **mode-line-name** — the short string shown in the mode line, e.g.
  `"Lisp Interaction"`.

Lookup of an inheritable field walks `mode → parent → …`: `(mode-field m
'on-return)` returns the first non-nil `on-return` up the parent chain.

`define-mode` registers a descriptor. `define-derived-mode` is sugar: it sets
`parent` and leaves inherited fields nil so they resolve up the chain.

### 3. The mode hierarchy (this spec)

There are **two orthogonal hierarchies**, and "graphics is the substrate" lives
in the first, not the second:

- **Rendering axis (C, the `kind` field).** Pixels are the base: `rd_core`
  writes pixels for everything. A surface buffer blits its canvas; a text buffer
  **rasterizes glyphs into the same framebuffer**. So "text is a graphic" is true
  at render time — it needs no mode to express it.
- **Behavior axis (Lisp, the major-mode tree).** This is about *what keys do*,
  inherited parent→child. The common base is not "surface" (that implies a pixel
  canvas a text buffer doesn't have) but **"displayed + inert"**; pixels and text
  editing are two specializations of it. This mirrors Emacs's `special-mode`
  (inert: image-mode, dired, help) vs `fundamental-mode` (editing) being shallow
  siblings — there is no "everything descends from image."

The mode tree:

```
special-mode            ; root: displayed, ignores input
  surface-mode          ; + a pixel canvas an external renderer draws into
  fundamental-mode      ; + text self-insert / RET
    text-mode
    repl-mode
    lisp-interaction-mode
```

- **special-mode** — root, `parent` nil.
  - `on-self-insert` = `ignore-char` → no-op; `on-return` = `ignore-ret` → no-op.
  - `keymap` nil; `setup-fn` nil; mode-line-name `"Special"`. An inert displayed
    buffer: stray keys do nothing, but ctrl/meta still fall through to
    `global-keymap` (so `C-x o` etc. still navigate away).
- **surface-mode** ← special-mode. A full-window **pixel canvas** (the teapot /
  any external renderer). Inherits the inert handlers (you can't type into a
  canvas); mode-line-name `"Surface"`. Set automatically by the
  `make-surface-buffer` Lisp wrapper (§9a).
- **fundamental-mode** ← special-mode. Adds text editing:
  - `on-self-insert` = `fundamental-self-insert` → `(insert (string-from-char ch))`
  - `on-return` = `fundamental-return` → `(insert "\n")`
  - `keymap` nil; `setup-fn` nil; mode-line-name `"Fundamental"`.
- **text-mode** ← fundamental. Inherits everything; mode-line-name `"Text"`,
  doc describes plain editing. What `C-x C-f` files open in.
- **repl-mode** ← fundamental.
  - `on-self-insert` = `repl-self-insert` → insert at point (motion clamps to the
    prompt via the existing `cur-start`).
  - `on-return` = `repl-return` → `repl-eval`.
  - `keymap` = `repl-keymap` binding C-p/Up `cmd-history-prev`, C-n/Down
    `cmd-history-next` (history is REPL-only; today these are gated by
    `editing-p` inside the global commands — move the gate into the mode keymap).
  - `setup-fn` = `repl-setup` → `(prompt)`.
  - mode-line-name `"REPL"`.
- **lisp-interaction-mode** ← fundamental.
  - `on-self-insert` inherited (insert); `on-return` inherited (newline).
  - `keymap` = `lisp-interaction-keymap` binding **C-j (`ctrl 106`, `j`=106 —
    the keymaps key ctrl bindings on the letter's ASCII code, as `global-keymap`
    does; this also keeps C-j distinct from RET, which arrives as `char 10`)** to
    `eval-last-sexp`.
  - `setup-fn` = `scratch-setup` → insert the `*scratch*` banner once.
  - mode-line-name `"Lisp Interaction"`. What `*scratch*` uses.

Explicitly **out of scope** (slot in later without rework): `prog-mode`, mode
hooks beyond `setup-fn`, mode-line *format strings*, minor modes.

### 4. Buffer-local variables

Generalize `repl-starts` / `file-of` into one store: an alist
`*buffer-locals*` of `(handle . plist)`.

- `(buffer-local-get buf sym default)` — plist lookup, `default` if absent.
- `(buffer-local-set buf sym val)` — upsert.

Buffer handles are fixnums (as today). The following become buffer-locals:

- `major-mode` — the buffer's mode symbol. `(buffer-mode buf)` =
  `(buffer-local-get buf 'major-mode 'fundamental-mode)`.
- `repl-start` — replaces the `repl-starts` alist (REPL prompt position).
- `file-path` — replaces the `file-of` alist (visited file).

`cur-start` / `set-cur-start` are reimplemented over `repl-start`; the
`file-of` accesses in `save-buffer` over `file-path`. The standalone
`repl-bufs` / `repl-buf-p` / `editing-p` go away: "is a REPL" becomes
`(eq (buffer-mode (current-buffer)) 'repl-mode)`, and "is editable" becomes
"not repl-mode" where still needed (`edit-lo`).

### 5. Entering a mode

`(set-major-mode buf mode-sym)`:

1. `(buffer-local-set buf 'major-mode mode-sym)`.
2. `(set-mode-line-name (mode-line-name-of mode-sym))` for the buffer (see §8).
3. Run the mode's `setup-fn` (resolved up the chain) with `buf` selected.

A small helper `(make-buffer-in name mode-sym)` makes a buffer, selects it, and
calls `set-major-mode`.

### 6. Dispatch, rewritten

`dispatch` resolves `m = (buffer-mode (current-buffer))` and routes:

- `null ev` → nil. `mb-action` / `pending-cx` / `pending-ch` / mouse / C-x / C-h
  / C-g / M-x handling stays exactly as today (global prefixes).
- A `ctrl` or `meta` key that is not a prefix: look it up with
  `(mode-key-lookup m ev)`, which walks the mode keymap chain and then
  `global-keymap`; run the binding if found, else fall through (echo "unbound").
  This replaces the bare `(run-key ev global-keymap)`.
- A `char` event:
  - RET (10) → `(mode-return m)` (resolved up the chain).
  - backspace (8) → mode-aware delete: in repl-mode clamp to `cur-start`;
    otherwise delete one char before point if any.
  - else → `(mode-self-insert m ch)`.

The keymap chain is the payoff: **C-j (`ctrl 106`) evaluates only in
lisp-interaction-mode**,
with no global special-casing, and REPL history keys live in repl-mode.

### 7. `eval-last-sexp` (lisp-interaction C-j)

`(eval-last-sexp)`:

1. Find the bounds of the last balanced form ending at/just before point:
   scan left from point over trailing whitespace, then match parens
   (`last-sexp-start`): walk back counting `)` and `(` until depth returns to 0,
   or to the start of an atom if point isn't after a `)`.
2. `src = (buffer-substring start point)`.
3. `(goto-char point)`, `(insert "\n")`, `val = (eval (read-string src))`,
   `(flush-output)`, `(insert (prin1-to-string val))`, `(insert "\n")`.

Reuses the REPL's read/eval/print, anchored at point instead of a prompt. No
prompt is inserted (lisp-interaction has none).

### 8. Mode line shows the mode

One small change in `src/rd_core.c`:

- Add `char mode_name[32];` to the buffer struct (the struct the renderer reads
  for `b->name`), default empty.
- Add a Lisp primitive `(set-mode-line-name str)` that copies `str` into the
  current buffer's `mode_name`.
- In the `modeline:` block, render `-- <name>  (<mode_name>) ---` when
  `mode_name` is non-empty, else the current `-- <name> ---`.

All mode-entry logic stays in Lisp; C only stores and paints the string.

### 9. `*scratch*` at boot

`frame-setup` creates **`*scratch*` in lisp-interaction-mode** as the initial
buffer (instead of marking the first buffer a `*repl*`). `scratch-setup` prints
a short banner. A `*repl*` is created on demand:

- `new-repl` makes a buffer and `(set-major-mode b 'repl-mode)` (its `setup-fn`
  prints the prompt). `C-x 2` / `C-x 3` still call `new-repl`.
- A `repl` command (M-x) opens a repl-mode buffer in the current window.

`frame-tick`'s error-recovery (today: always `(insert pending) (insert "\n")
(prompt)`) becomes mode-aware: insert the pending output, then only re-prompt if
the current buffer's mode is repl-mode.

A `repl-here` command (bound to `C-x r`) turns the current window's buffer into
a fresh REPL (clear + `set-major-mode … 'repl-mode`), so a REPL is one chord
away from `*scratch*`. `new-repl` (used by `C-x 2` / `C-x 3`) likewise sets
repl-mode.

### 9a. `surface-mode` for graphical buffers

Surface buffers (the teapot, any external renderer) are unchanged mechanically:
`make-surface-buffer` (C) still allocates an shm pixel canvas marked
`RD_SURFACE`, an external program draws into it, and `rd_core` blits it. The
only addition brings them into the mode system: a thin **Lisp wrapper**
`(make-surface name w h)` calls the C `make-surface-buffer` primitive and then
`(set-major-mode b 'surface-mode)`. `teapot` (and any future renderer) uses the
wrapper. Result: selecting `*teapot*` and typing does nothing (surface-mode
inherits `ignore-char`/`ignore-ret` from special-mode) instead of corrupting the
hidden text store, the mode line reads `(Surface)`, and `C-h m` describes it —
uniform with text buffers.

(Future, Spec 2: a *surface anchored to a text span* — `display`-property on a
text run pointing at a surface handle — is how **inline graphics in a text
buffer** will work. Surfaces become the unifying graphics primitive: full-window
(surface-mode) or inline (a text property). That needs the text-property layer
plus an `rd_core` change to blit a canvas across K×M cells during text layout,
so it lives in the DolDoc/text-properties follow-on, not here.)

### 10. `C-x C-f` interactive

- New minibuffer action `'find-file`: `mb-start` sets it; `mb-source` returns
  nil for this action (no candidate list — **free-text path entry**); `mb-render`
  shows `Find file: <input>` with no candidate slice; `mb-commit` for
  `'find-file` calls `(find-file-here (read-string-path mb-input))`.
- `(find-file-here path)`: switch **the current window's** buffer to a new
  `text-mode` buffer (`make-buffer-in path 'text-mode`), read the file in with
  `open`/`fd-read` (missing file → empty buffer), `(buffer-local-set b
  'file-path path)`, `(goto-char 0)`, `(redisplay)`. No auto-split.
- Bind `C-x C-f` (`ctrl 102`, `f`=102) in `cx-keymap` to a command
  `cmd-find-file` → `(mb-start 'find-file)`.
- `save-buffer` reads `file-path` from buffer-locals (§4); unchanged behavior.

The minibuffer generalization: `mb-source`/`mb-filter`/`mb-render`/`mb-commit`
gain a `'find-file` branch that bypasses candidate filtering and treats
`mb-input` as the literal result.

### 11. Self-documenting help follows automatically

- `describe-mode` (C-h m) reads the **current buffer's mode**: its `doc`, its
  mode-line-name, and `fmt-bindings` over its keymap chain + globals. No more
  hardcoded "File buffer" / "REPL" `if`.
- `describe-bindings` (C-h b) additionally lists the current mode's keymap.

## Data flow

```
key event ──► dispatch ──► m = (buffer-mode (current-buffer))
                          │
                          ├─ printable ─► (mode-self-insert m ch)
                          ├─ RET ───────► (mode-return m)
                          └─ ctrl/meta ─► (mode-key-lookup m ev)  ─► run command
                                            │ walks mode→parent→global keymaps
set-major-mode ─► buffer-local-set major-mode
               ─► set-mode-line-name  ─► (C) paints "-- name (Mode) --"
               ─► run setup-fn
```

## Error handling

- Unknown key in a mode keymap chain → echo "unbound" (today's behavior).
- `find-file` on a missing path → empty text-mode buffer (create-on-save), as
  today.
- `save-buffer` with no `file-path` → echo "no file for this buffer".
- `eval-last-sexp` on an empty/unbalanced region → the existing per-tick C-level
  error recovery shows the error and continues; no prompt is inserted in
  lisp-interaction-mode.
- Entering an unregistered mode symbol → falls back to fundamental-mode
  (registry lookup returns nil → inherited fields resolve to fundamental).

## Testing (TDD)

Two layers, both already established in the repo.

**Serial-REPL unit tests — `tools/modes_check.py` (fast, deterministic).** The
plain serial REPL loads `bootstrap → system → modes` (not frame). Send forms,
assert printed results:

- `define-derived-mode` sets the parent; `(mode-field 'text-mode 'on-self-insert)`
  resolves up to `fundamental-self-insert`.
- `(mode-key-lookup 'lisp-interaction-mode '(ctrl 106))` finds `eval-last-sexp`;
  the same lookup in `text-mode` does not.
- `buffer-local-set` / `buffer-local-get` round-trip; `default` returned when
  absent; two handles stay independent.
- `(buffer-mode <fresh-handle>)` defaults to `fundamental-mode`.

(Requires `modes.l` to load in the serial REPL — i.e. it must not reference
frame/gfx primitives at load time. The pure helpers satisfy this.)

**Frame OCR/QMP tests** (drive the frame via injected keyboard events + a
screendump, OCR'd against `font_aa.h`, like the Phase-27 checks):

- `tools/scratch_check.py`: boot → the mode line of the initial window reads
  `(Lisp Interaction)`; the `*scratch*` banner is present.
- `tools/lispinteraction_check.py`: type `(+ 1 2)`, press `C-j`, the buffer
  shows `3` on the next line.
- `tools/textmode_check.py`: `C-x C-f`, type a `/disk/...` path, RET, type text,
  `C-x C-s`; reopen the same path in a fresh boot (persistent disk) and verify
  the text is present; mode line reads `(Text)`.
- `tools/replmode_check.py`: `C-x r` opens a repl-mode window whose mode line
  reads `(REPL)` and whose RET evaluates.
- `tools/surface_check.py`: `(teapot)` (via a REPL) opens `*teapot*` whose mode
  line reads `(Surface)`.

Existing KTESTs go 155 → **156** (one new `rd: modeline shows mode name`).
Changing the frame's default buffer from a REPL to `*scratch*` changes
RET-evaluates-here, so the ~13 existing frame checks that type a form+RET get a
one-line "open a REPL here" (`C-x r`) preamble — that fallout is handled
explicitly in the plan, not waved away.

## Risks

- **Dispatch rewrite is central.** Every keystroke flows through it. Mitigation:
  keep the prefix/minibuffer/mouse branches byte-for-byte; only the
  char/key-routing tail changes. The frame OCR checks (existing + new) are the
  net.
- **`modes.l` must stay graphics-free** so the serial REPL can load it for unit
  tests. Keep `current-buffer`/`insert`/`make-buffer` out of `modes.l`; those
  live in `frame.l` wiring.
- **`eval-last-sexp` paren scanning** is the only nontrivial new parser. Keep it
  simple (balance parens from point leftward); a malformed region falls into the
  existing per-tick error recovery rather than crashing the loop.
- **Removing `repl-bufs`** touches several call sites (`cur-start`, `edit-lo`,
  `new-repl`, `frame-setup`, `frame-tick`). Grep-and-replace against the
  buffer-mode predicate; the replmode/scratch checks guard the result.

## Decomposition

One spec, one plan — a single subsystem (the frame's mode dispatch). The plan
will sequence it as: (1) `modes.l` pure model + serial tests; (2) the
`rd_core.c` mode-line field + primitive; (3) rewire `frame.l` dispatch + boot
onto modes; (4) `lisp-interaction-mode` + `eval-last-sexp`; (5) interactive
`C-x C-f`; (6) self-documenting help follows. Each step keeps the suite green.
