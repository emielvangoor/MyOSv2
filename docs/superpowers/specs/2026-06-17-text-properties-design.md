# Text Properties + ANSI Translation — Design

**Date:** 2026-06-17
**Status:** Approved (build to completion)
**Goal:** Give MyOSv2 buffers genuine **Emacs-style text properties** (arbitrary properties over character ranges, with `face` as the display property), and translate Unix/ANSI SGR escape sequences into `face` text properties so program output (e.g. `busybox ls`) renders **colored** in the frame instead of showing literal `?[1;34m` escape bytes.

Follow Emacs design closely: the API names and semantics mirror Emacs (`put-text-property`, `get-text-property`, `propertize`, `set-text-properties`, named `face`s, an `ansi-color`-style filter).

---

## Problem

The frame's redisplay engine (`src/rd_core.c`) stores each buffer as a gap buffer of plain `char`s. The screen is a grid of `rd_cell { unsigned char ch, face; }` where `face` indexes a fixed 8-entry table of `rd_face { uint32_t fg, bg; }`. Buffer text always paints with face 0; the mode line/cursor/selection get faces 1/2 applied **positionally** by the renderer. There is no way to say "this character has a face."

So when busybox emits `\x1b[1;34mbin\x1b[m`, those bytes are inserted as literal text and rendered as `?[1;34mbin?[m` (see the reported screenshot). We need (1) a text-property layer on buffers and (2) an ANSI→`face` filter on program output.

---

## Component 1 — Text properties: buffer-owned intervals (the root primitive)

Each `struct rd_buffer` gains a **text-property interval list**: a small dynamic array (or linked list) of `struct rd_interval { int start, end; Lobj plist; }` in **text coordinates** (the same coordinates as `point`, i.e. logical character positions, gap-independent). `plist` is an ordinary Lisp property list, e.g. `(face ansi-blue)`.

Semantics (Emacs-faithful):
- Properties attach to characters in `[start, end)`. A position with no covering interval has no properties (the `nil` plist).
- **Insert** at position `p` of `n` chars: every interval boundary `>= p` shifts by `+n`. Newly inserted text gets the properties carried with it (see Component 2's propertized insert) or none.
- **Delete** `[p, p+n)`: boundaries `>= p+n` shift by `-n`; boundaries inside the deleted range clamp to `p`; intervals that become empty are dropped.
- Adjacent intervals with `equal` plists may be coalesced (optional optimization; not required for correctness).
- `put-text-property start end prop val` splits/updates intervals so `[start,end)` carries `prop=val`, preserving other props already there (a property merge, not a wholesale replace). `set-text-properties` replaces the whole plist over the range. `remove-text-properties` removes named props.

**GC integration (the faithful, load-bearing piece):** interval `plist`s are live Lisp objects reachable only from buffers, so the mark-sweep GC must trace them. `lm_core.c`'s root phase (currently marks `symtab`, `global_env`, `env`, + conservative stack at lines ~306-308) calls a new hook `gfx_gc_mark_buffers()` (implemented in `lm_gfx.c`, which owns the `bufs[]` array) that, for every used buffer, calls `gc_mark(interval->plist)` for each interval. Without this, a GC between `put-text-property` and redisplay would free face plists. This mirrors real Emacs, where a buffer's text properties are part of the GC graph.

This store and its operations live in `rd_core.c` alongside the gap buffer (they must stay consistent under the same insert/delete). The enumeration-for-GC hook lives in `lm_gfx.c`.

## Component 2 — Lisp API (mirrors Emacs)

New primitives (in `user/lm_gfx.c`, operating on the current or a named buffer):
- `(put-text-property START END PROP VAL)` — set one property over `[START,END)` in the current buffer.
- `(get-text-property POS PROP)` — the value of PROP at POS (or `nil`).
- `(set-text-properties START END PLIST)` — replace the property list over the range.
- `(remove-text-properties START END PLIST)` — remove the named properties (values in PLIST ignored, Emacs-style).
- `(text-properties-at POS)` — the full plist at POS.
- `(propertize STRING PROP VAL ...)` — returns a **propertized string**: a string object carrying a parallel property record. Since this Lisp's strings are simple, represent a propertized string as a tagged pair `(cons :propertized (cons STRING PLIST-OR-INTERVALS))` OR extend the string object with an optional property side-channel — **decision: a lightweight wrapper** `(propertized-string RAW INTERVALS)` recognized by `insert`. (Implementation picks the least invasive representation; the API behaves like Emacs `propertize` for the common "whole string, uniform props" case, which is all `ansi-color-apply` needs per span.)
- `(insert X)` — extended: if X is a propertized string, append its raw text AND apply its properties to the inserted range; if X is a plain string, append with no properties (unchanged behavior).

`buffer-substring` may later grow a `buffer-substring` (no-props, current) vs a propertized variant — **out of scope** unless needed.

## Component 3 — Named, themeable faces

Replace the fixed 8-slot numeric face model with **named faces**:
- A face registry: name (symbol) → attributes `{ uint32_t fg, bg; int bold, inverse, underline; int fg_set, bg_set; }`. `fg_set`/`bg_set` let a face specify only fg (inherit bg), needed for ANSI (color sets fg, keeps the buffer bg).
- `(defface NAME :foreground FG :background BG :bold BOOL ...)` / `(set-face-attribute NAME ...)` register/update faces. Colors are `0x00RRGGBB` fixnums (matching the existing `set-face`).
- The existing built-ins become named: `default`, `mode-line`, `cursor`, `region` (current faces 0/1/2 + selection). `set-face` (the legacy numeric primitive) is kept working for back-compat or reimplemented over the registry.
- The `face` **text-property value** is a face name (symbol) **or a list of face names** merged left-to-right (later faces override earlier set attributes), exactly like Emacs.

**Renderer resolution:** `rd_cell` keeps a small integer face id. The frame maintains a resolved-face table (grow `RD_NFACES`, e.g. to 64). When redisplay needs to paint a character whose `face` property resolves to a concrete `{fg,bg,attr}` combo, it **find-or-allocates** a cell-face id for that combo (a small cache keyed by the resolved attributes), so the unbounded space of merged faces collapses onto a bounded set of actually-used cells. (ANSI uses a small, bounded set in practice.) Default text → the `default` face id.

## Component 4 — ANSI → text properties (`ansi-color` analog)

- A **pre-registered ANSI palette** (named faces), themeable via the registry: `ansi-black ansi-red ansi-green ansi-yellow ansi-blue ansi-magenta ansi-cyan ansi-white`, the `ansi-bright-*` set, and attribute faces `ansi-bold`, `ansi-inverse`, `ansi-underline`. Default colors approximate the current Gruvbox-ish theme; the user can recolor them.
- `(ansi-color-apply STRING)` — the `ansi-color.el` analog. Parses `ESC [ params m` (SGR) sequences, maintaining current-style state:
  - `0` (or empty) → reset to no face; `1` → add bold; `4` → underline; `7` → inverse; `30–37` → fg `ansi-<color>`; `90–97` → fg `ansi-bright-<color>`; `40–47`/`100–107` → bg; `39`/`49` → reset fg/bg.
  - Emits a **propertized string**: the escape bytes are **stripped**, and each run of text carries a `face` property = the list of active ANSI faces.
  - Non-`m` CSI sequences (cursor movement `H`/`J`/`K`, etc.) and other escapes are **stripped and ignored** (no terminal emulation — this is about text styling).
- **Stateful across chunks:** program output streams in fixed-size chunks (`fd-read ... 1024`), so an escape can split across a chunk boundary. `ansi-color-apply` keeps state in a dynamic variable (e.g. `*ansi-state*`): the in-progress escape fragment and the current active faces, carried between calls — exactly how comint's `ansi-color-process-output` works.
- **Wiring:** `stream-thunk` (in `user/lisp/frame.l`) currently does `(insert chunk)`. It becomes `(insert (ansi-color-apply chunk))`. The REPL banner/results path is plain text and unchanged.

## Component 5 — Renderer integration

In `render_leaf` (`rd_core.c`), where buffer text is painted via `put_cell(..., face)`, replace the hardcoded face 0 with a lookup: for the character at text position `p`, get its `face` text property (walk the interval list), resolve it through the face registry to a cell face id, and paint with that. Mode line, cursor, and selection keep their positional faces (now referenced by name). A buffer with no text properties paints exactly as today (default face) — zero visual change until properties exist.

---

## Testing (TDD)

**KTESTs (`src/tests.c`)** — the rd engine + Lisp core run in-kernel under `make test`:
- Interval store: `put-text-property` then `get-text-property` returns the value; a property set on `[2,5)` is absent at 1 and 5; **insert** before a propertized range shifts it; **delete** spanning a range clamps/drops it; two `put`s of different props on overlapping ranges both resolve (merge).
- GC: put a `face` property, force a `gc_collect()`, confirm `get-text-property` still returns it (the plist survived because the buffer traced it).
- `propertize` + `insert`: inserting a propertized string lands the text and the property at the right range.
- SGR parser: `ansi-color-apply "\x1b[1;34mX\x1b[m"` returns text `"X"` (escapes stripped) with `face` containing `ansi-blue`+`ansi-bold`; a sequence split across two calls (`"\x1b[1;3"` then `"4mX\x1b[m"`) still produces the right result (state carried).
- Face registry: `defface`/`set-face-attribute` then resolve merged faces (`(ansi-bold ansi-blue)`) to the expected fg + bold.

**Frame integration check (`tools/textprops_check.py`, new):** boot the frame, run `busybox ls /` in a buffer, screenshot, and assert (a) a known directory entry renders in the ANSI fg color (sample the glyph pixels via the existing PPM helpers / the frame_check face decoding), and (b) **no literal `?[` / `[1;34m` bytes** appear in the buffer text (read it back). Confirms the escapes were translated, not displayed.

Existing checks (`autostart`, `frameedit`, `findfile`, KTEST suite) must stay green — a buffer with no properties must render identically to before.

---

## Out of scope
- Full terminal emulation (cursor positioning, screen clearing, scroll regions, alternate screen). Only SGR styling is translated; other control sequences are stripped.
- 256-color / 24-bit (`38;5;n` / `38;2;r;g;b`) SGR — recognized-and-skipped now; can extend the palette later.
- `mouse-face`, `display`, `read-only`, `keymap` and other text properties as *behaviors* — the general mechanism stores any property, but only `face` is consumed by the renderer in this phase.
- Propertized `buffer-substring`, kill/yank carrying properties.
- Text properties on the echo area / minibuffer (buffers only).
