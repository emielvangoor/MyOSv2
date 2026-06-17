# Text Properties + ANSI Translation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).

**Goal:** Emacs-style text properties on buffers (`put/get/set/remove-text-properties`, `propertize`, `insert` carries them), named themeable faces, the renderer painting each character with its `face` property, and an `ansi-color-apply` filter that turns SGR escapes into `face` properties so `busybox ls` renders colored in the frame.

**Architecture:** Text-property interval list on `struct rd_buffer` (in `src/rd_core.c`, compiled into the **user** `lm.elf`), traced by the lm GC; Lisp primitives in `user/lm_gfx.c`; a named-face registry; ANSI parsing in Lisp (`ansi-color-apply`) wired into `frame.l`'s output stream.

**Tech stack:** C (freestanding, the lm Lisp machine in `lm.elf`), the `rd` redisplay engine, the lm Lisp dialect, Python QEMU integration checks. Reference: `docs/superpowers/specs/2026-06-17-text-properties-design.md`.

**Testing reality (important):** `src/rd_core.c`, `src/lm_core.c`, `user/lm*.c` compile into `lm.elf` (the userland Lisp machine), **not** the kernel — so the kernel KTEST harness (`make test`) cannot call these functions. They are tested through:
- **The Lisp REPL**, scripted over the network REPL (`lisp -serve`, via `lm_harness.connect_repl` / `repl_roundtrip`) or the serial REPL — send Lisp expressions, assert on the printed result. This is the real API contract.
- **Frame screenshots** (QMP + the PPM/face helpers in `frame_check.py`) for color rendering.

`make test` (KTESTs) must still pass — it builds the kernel **and** `build/disk.img` (which builds `lm.elf`), so any compile error in `lm.elf` fails the gate. The pre-commit hook runs it.

---

## File Structure

| File | Change |
|---|---|
| `src/rd.h` | `struct rd_interval`; intervals on `struct rd_buffer`; `RD_NFACES` grows; face attrs; decls for the new rd ops (uses `Lobj` from `lm.h`) |
| `src/rd_core.c` | interval store (put/get/set/remove/at), insert/delete adjustment in the gap-buffer ops, GC-mark helper, face-registry resolution, `render_leaf` reads `face` |
| `user/lm_gfx.c` | Lisp primitives: `put-text-property`/`get-text-property`/`set-text-properties`/`remove-text-properties`/`text-properties-at`/`propertize`; extend `insert`; `defface`/`set-face-attribute`; `gfx_gc_mark_buffers()` |
| `src/lm_core.c` | call `gfx_gc_mark_buffers()` from the GC root phase |
| `user/lisp/frame.l` | `ansi-color-apply` + ANSI palette faces; `stream-thunk` runs chunks through it |
| `tools/textprops_check.py` | new integration check (REPL API + frame color) |
| `README.md`, `docs/ROADMAP.md` | reflect text properties |

---

## Task 1: Text-property interval store + core Lisp API

**Files:** `src/rd.h`, `src/rd_core.c`, `user/lm_gfx.c`, `src/lm_core.c`. Test via the REPL.

- [ ] **Step 1: Define the interval struct and buffer field (`src/rd.h`)**

`rd.h` is compiled inside `lm.elf` (with `lm.h` available). Add near the top a reference to `Lobj` — `#include "lm.h"` if not already included transitively, else forward-use. Add:
```c
struct rd_interval { int start, end; Lobj plist; };   // text-coordinate [start,end) -> plist
#define RD_MAX_INTERVALS 256
```
Add to `struct rd_buffer`:
```c
    struct rd_interval ivals[RD_MAX_INTERVALS];
    int n_ivals;
```
(A fixed array keeps allocation simple and GC tracing trivial; 256 spans/buffer is plenty for ls-style output. If a buffer overflows, `put-text-property` silently no-ops the excess — acceptable.)

Declare the rd ops (add to `rd.h`):
```c
void rd_put_text_prop(struct rd_buffer *b, int start, int end, Lobj prop, Lobj val);
Lobj rd_get_text_prop(struct rd_buffer *b, int pos, Lobj prop);
Lobj rd_text_props_at(struct rd_buffer *b, int pos);
void rd_set_text_props(struct rd_buffer *b, int start, int end, Lobj plist);
void rd_remove_text_props(struct rd_buffer *b, int start, int end, Lobj props);
void rd_buf_mark_props(struct rd_buffer *b, void (*mark)(Lobj));  // GC
```

**Verify `rd.h` is not included by the kernel build** (grep `src/*.c` outside the lm set for `#include "rd.h"`). It should only be in the lm/rd files. If the kernel includes it, guard the `Lobj` parts under the lm build. (It does not today.)

- [ ] **Step 2: Implement the interval store (`src/rd_core.c`)**

Implement the six functions. Key behaviors (plist values are Lisp objects; use the lm core's `cons`, `CAR`, `CDR`, `Qnil`, `eq`, plist helpers — read `src/lm.h`/`lm_core.c` for the exact names):
- `rd_get_text_prop(b, pos, prop)`: find the interval covering `pos` (start <= pos < end); walk its plist for `prop`; return the value or `Qnil`.
- `rd_text_props_at(b, pos)`: return the covering interval's plist (or `Qnil`).
- `rd_put_text_prop(b, start, end, prop, val)`: ensure `[start,end)` is covered by intervals whose plist has `prop=val` merged in (other props preserved). Simplest correct approach: split existing intervals at `start` and `end`, then for each interval fully inside `[start,end)` update its plist (replace prop's value, or prepend `prop val`); create a new interval for any uncovered sub-range. Keep intervals sorted, non-overlapping.
- `rd_set_text_props`: same coverage, but each interval's plist becomes a copy of `plist`.
- `rd_remove_text_props`: remove named props from intervals in range; drop intervals whose plist becomes empty.
- `rd_buf_mark_props(b, mark)`: `for each interval: mark(b->ivals[i].plist);`

Helper `plist_get(Lobj plist, Lobj key)` and `plist_put(Lobj plist, Lobj key, Lobj val)` (returns a new plist) — implement locally or reuse if the lm core has them.

- [ ] **Step 3: Adjust intervals on insert/delete (`src/rd_core.c`)**

In `rd_buf_insert(b, s)` (inserts at point): after computing the inserted length `n` at text position `p = b->point` (in text coords, before/after per the existing code), shift every interval boundary `>= p` by `+n`. (A boundary exactly at `p`: a `start == p` shifts to keep inserted text *outside* the following interval by default — Emacs inserts with the inserted text inheriting no properties unless stipulated; choose `start == p` shifts `+n` so inserted text is property-free, matching plain `insert`.)
In the buffer's delete path (find the delete function, e.g. `rd_buf_delete`/`delete-char`): for a delete of `[p, p+n)`, for each interval: clamp `start`/`end` into the surviving coordinate space (`x >= p+n → x-n`; `p < x < p+n → p`; `x <= p → x`), drop intervals that become empty (`start >= end`).

Add a tiny KTEST-able-by-REPL note: these run during normal editing/streaming.

- [ ] **Step 4: Lisp primitives (`user/lm_gfx.c`)**

Add (using the `DEFGFX`/arg-accessor idioms already in the file; `cur()` is the current buffer):
```c
// (put-text-property START END PROP VAL) -> nil
DEFGFX("put-text-property", Gput_tp, 4, 4) { ...
    rd_put_text_prop(cur(), (int)req_fixnum(a0,...), (int)req_fixnum(a1,...), a2, a3); return Qnil; }
// (get-text-property POS PROP) -> val|nil
DEFGFX("get-text-property", Gget_tp, 2, 2) { ... return rd_get_text_prop(cur(), pos, prop); }
// (text-properties-at POS) -> plist
// (set-text-properties START END PLIST) -> nil
// (remove-text-properties START END PLIST) -> nil
```
Register them in the `register_*` list. Match the file's existing fixnum/string accessors and registration pattern.

- [ ] **Step 5: GC tracing (`user/lm_gfx.c` + `src/lm_core.c`)**

In `user/lm_gfx.c` add:
```c
// Mark every buffer's text-property plists so a GC between put-text-property and
// redisplay can't free a face. The buffers live in this file's bufs[] array.
void gfx_gc_mark_buffers(void)
{
    for (int i = 0; i < NBUFS; i++) {
        if (buf_used[i]) { rd_buf_mark_props(&bufs[i], gc_mark); }
    }
}
```
Declare `void gfx_gc_mark_buffers(void);` and `void gc_mark(Lobj);` where lm_core can see them (a shared header, e.g. `lm.h`, or an `extern` in lm_core.c). In `src/lm_core.c`'s GC root phase (after `gc_mark(global_env); gc_mark(env);`, ~line 308), add `gfx_gc_mark_buffers();`. To avoid a hard link dependency when lm is built without the gfx unit, you may guard with a weak symbol or always link gfx (lm.elf always includes lm_gfx.c — confirm in the Makefile; it does). Also handle the frame's own buffers if the frame keeps a separate `frame.bufs` — confirm `bufs[]` is the single buffer array.

- [ ] **Step 6: Test via the REPL (red → green)**

Build, boot serve REPL, and check (write `tools/_tp1_probe.py` or extend the final check). Expressions and expected:
```
(progn (insert "hello") (put-text-property 0 3 'face 'myface) (get-text-property 1 'face))  => myface
(get-text-property 4 'face)   => nil
(progn (goto-char 0) (insert "XX") (get-text-property 3 'face))  => myface   ; shifted by insert
```
Before Step 2-4 these error (unbound `put-text-property`); after, they return as shown. Use `lm_harness.connect_repl`/`repl_roundtrip` (start with `(run "lisp" "-serve")` via `boot_to_serve`, or send over serial). Confirm `make test` still green (compilation gate).

- [ ] **Step 7: Commit**
```bash
git add src/rd.h src/rd_core.c user/lm_gfx.c src/lm_core.c
git commit -m "feat(textprops): buffer text-property intervals + put/get/set/remove API + GC tracing"
```

---

## Task 2: propertize + insert carries properties

**Files:** `user/lm_gfx.c` (and a representation helper). Test via REPL.

- [ ] **Step 1: Choose the propertized-string representation**

A propertized string is `(cons sym_propertized (cons RAW PLIST))` where `sym_propertized` is the interned symbol `propertized-string` and `RAW` is the plain string, `PLIST` the uniform property list applied to the whole string. (Uniform-whole-string properties are all `ansi-color-apply` needs per emitted span.) Add a predicate `is_propertized(Lobj)`.

- [ ] **Step 2: `(propertize STRING PROP VAL ...)` (`user/lm_gfx.c`)**

Build the PLIST from the `PROP VAL ...` rest args, return `(propertized-string RAW PLIST)`. Register.

- [ ] **Step 3: Extend `(insert X)` (`user/lm_gfx.c`)**

In `Ginsert`: if `X` is a propertized string, get its RAW + PLIST, record `start = point`, `rd_buf_insert(cur(), raw)`, then `rd_set_text_props(cur(), start, point, plist)` (apply the uniform plist to the inserted range). If `X` is a plain string, unchanged. (Point advances by the raw length on insert — confirm `rd_buf_insert` moves point; the existing code does.)

- [ ] **Step 4: Test (REPL)**
```
(progn (goto-char 0) (delete-char (buffer-length)) (insert (propertize "hi" 'face 'blue)) (get-text-property 0 'face))  => blue
(get-text-property 1 'face) => blue
```

- [ ] **Step 5: Commit**
```bash
git add user/lm_gfx.c
git commit -m "feat(textprops): propertize + insert carries text properties"
```

---

## Task 3: Named, themeable faces

**Files:** `src/rd.h`, `src/rd_core.c`, `user/lm_gfx.c`. Test via REPL.

- [ ] **Step 1: Grow + restructure the face model (`src/rd.h`)**

```c
#define RD_NFACES 64
struct rd_face { uint32_t fg, bg; unsigned char bold, inverse, underline, fg_set, bg_set; };
```
Keep `front`/`back` `rd_cell.face` as the cell face id (now an index 0..63). (If `rd_cell.face` is `unsigned char`, 64 fits.)

- [ ] **Step 2: Named-face registry (`user/lm_gfx.c`)**

A Lisp-level alist `*faces*` mapping a face name (symbol) → a fixnum face id into `frame.faces[]`, plus the C `frame.faces[id]` attributes. Built-ins registered at init: `default` (id 0, current colors), `mode-line` (id 1), `cursor`/`region` (id 2). Primitives:
- `(defface NAME :foreground FG :background BG :bold B :inverse I :underline U)` — allocate/find an id, fill `frame.faces[id]`, add to `*faces*`. Missing keys leave defaults (`fg_set`/`bg_set` 0).
- `(set-face-attribute NAME ...)` — update an existing face.
- Keep the legacy `(set-face id fg bg)` working (it writes `frame.faces[id]` directly).
- `(face-id NAME)` → the fixnum id (helper for the renderer/tests).

- [ ] **Step 3: Face resolution (`src/rd_core.c`)**

A function `int rd_resolve_face(Lobj face_value)` that maps a `face` property value to a cell face id:
- a symbol → its registered id (via a C-visible lookup; expose the `*faces*` map to C, or have lm_gfx provide `int gfx_face_id(Lobj name)`).
- a list of symbols → merge: start from `default`, apply each named face's set attributes in order, producing a concrete `{fg,bg,attr}`; **find-or-allocate** a cell id for that combo in `frame.faces[]` (a linear scan for an equal entry, else claim the next free id; if full, fall back to the last entry). Return the id.
- `Qnil`/unknown → `default` (0).
Expose whatever lm_gfx→rd hook is needed for name→id (e.g. a function pointer set at init, or `gfx_face_id` declared in a shared header).

- [ ] **Step 4: Test (REPL)**
```
(defface 'tp-test :foreground #x00FF0000 :bold t)
(face-id 'tp-test)   => a fixnum >= 3
```
(Rendering is verified in Task 4.)

- [ ] **Step 5: Commit**
```bash
git add src/rd.h src/rd_core.c user/lm_gfx.c
git commit -m "feat(faces): named themeable face registry (defface/set-face-attribute) + merge/resolve"
```

---

## Task 4: Renderer paints each char with its face property

**Files:** `src/rd_core.c`. Test via frame screenshot.

- [ ] **Step 1: Read the face property in `render_leaf`**

Where buffer text is painted (the loop calling `put_cell(f, col, row, ch, FACE)` for text — currently FACE is 0/hardcoded), compute the per-character face: get the char's `face` text property (`rd_get_text_prop(b, p, sym_face)`), resolve via `rd_resolve_face`, pass that id to `put_cell`. Cache the symbol `face` (intern once). Mode line / cursor / selection unchanged (their named ids).

- [ ] **Step 2: Manual + screenshot test**

Build, boot the frame. In `*scratch*` evaluate:
```
(progn (defface 'tp-red :foreground #x00FF0000)
       (goto-char (buffer-length))
       (let ((s (buffer-length))) (insert "REDWORD")
            (put-text-property s (+ s 7) 'face 'tp-red))
       (redisplay))
```
Screenshot; assert the `REDWORD` glyph pixels are red (sample with `frame_check` PPM helpers). A buffer with no properties must look identical to before (regression).

- [ ] **Step 3: Commit**
```bash
git add src/rd_core.c
git commit -m "feat(textprops): redisplay paints each character with its face text property"
```

---

## Task 5: ANSI → text properties (`ansi-color-apply`) + wire into the stream

**Files:** `user/lisp/frame.l`. Test via REPL + frame screenshot.

- [ ] **Step 1: ANSI palette faces (`frame.l`, at load)**

`(defface 'ansi-black ...)` … `'ansi-white`, `'ansi-bright-black` … `'ansi-bright-white` (16 colors; pick a readable palette over the current bg), and attribute faces `'ansi-bold` (`:bold t`), `'ansi-inverse` (`:inverse t`), `'ansi-underline` (`:underline t`). Use the existing theme's colors where sensible.

- [ ] **Step 2: `ansi-color-apply` (`frame.l`)**

```
(setq *ansi-faces* nil)      ; active faces (list of symbols), carried across chunks
(setq *ansi-frag* "")        ; partial escape fragment carried across chunks
```
`(defun ansi-color-apply (s) ...)`: scan `s` (prepend `*ansi-frag*`), splitting into runs of literal text and SGR sequences `ESC [ params m`:
- On a complete `ESC [ ... m`: update `*ansi-faces*` per the params (`0`/empty → nil; `1`→add `ansi-bold`; `4`→`ansi-underline`; `7`→`ansi-inverse`; `30..37`→set fg to `ansi-<c>` (replace any prior ansi fg in the list); `90..97`→`ansi-bright-<c>`; `40..47`/`100..107`→bg faces; `39`/`49`→drop fg/bg; unknown params ignored).
- On other `ESC [ ... <final>` (non-`m`) or other escapes: strip and ignore.
- On a trailing INCOMPLETE escape at end of `s`: stash it in `*ansi-frag*` and stop.
- Emit: build the result by, for each literal run, appending either the raw text (if `*ansi-faces*` is nil) or `(propertize run 'face (copy *ansi-faces*))`. Since `insert` takes one object, return a **list** of strings/propertized-strings, OR concatenate — simplest: have `ansi-color-apply` return a list of pieces and the caller insert each. **Decision:** return a list; `stream-thunk` does `(foreach insert (ansi-color-apply chunk))`.

(Keep it byte-oriented; ESC is char 27. Reset `*ansi-faces*`/`*ansi-frag*` at the start of each `stream-thunk` run so state doesn't leak between programs.)

- [ ] **Step 3: Wire into `stream-thunk` (`frame.l`)**

Replace `(insert chunk) (redisplay)` with `(foreach insert (ansi-color-apply chunk)) (redisplay)`. Reset `*ansi-faces*`/`*ansi-frag*` before the read loop. Leave the line-editor keystroke inserts (plain) unchanged.

- [ ] **Step 4: Test (REPL + frame)**
- REPL: `(ansi-color-apply (string-concat (string-from-char 27) "[1;34mX" (string-from-char 27) "[m"))` returns a list whose `X` piece is a propertized string with `face` = `(ansi-blue ansi-bold)` (order per your impl) — assert by inserting it and reading back `get-text-property`, and that no `27`/`[` bytes remain in the buffer.
- Frame: run `busybox ls /` in a buffer; screenshot; assert a dir entry renders in the ANSI blue (sample pixels) and the buffer text contains `bin` but not `[1;34m`.

- [ ] **Step 5: Commit**
```bash
git add user/lisp/frame.l
git commit -m "feat(ansi): ansi-color-apply translates SGR escapes to face text properties; wire into stream"
```

---

## Task 6: Integration check + docs

**Files:** `tools/textprops_check.py` (new), `README.md`, `docs/ROADMAP.md`.

- [ ] **Step 1: `tools/textprops_check.py`**

Two parts: (a) API via REPL — boot, drive the REPL, assert `put`/`get`/`propertize`/`ansi-color-apply` behave (escape stripped, face applied); (b) frame color — run `busybox ls /` in the frame, screenshot, assert a colored entry and no literal `?[` bytes. Reuse `lm_harness` (`Qemu`, `connect_repl`/serial expect, `qmp_screendump`, `frame_check` helpers). Print `TEXTPROPS OK` on success.

- [ ] **Step 2: Run it green**
`make build/kernel.elf build/disk.img && python3 tools/textprops_check.py` → `TEXTPROPS OK`. Iterate on the implementation tasks if anything is off.

- [ ] **Step 3: Docs**
README: a bullet — buffers have **Emacs text properties** (`put/get-text-property`, `propertize`, named faces), and program output's ANSI colors are translated to `face` properties (colored `ls`). ROADMAP: append a "Text properties + ANSI" entry.

- [ ] **Step 4: Commit**
```bash
git add tools/textprops_check.py README.md docs/ROADMAP.md
git commit -m "test(textprops): integration check + README/roadmap for text properties + ANSI"
```

---

## Final verification
- [ ] `make clean && make test` green (compile + KTESTs).
- [ ] `python3 tools/textprops_check.py` → `TEXTPROPS OK`.
- [ ] `python3 tools/autostart_check.py`, `frameedit_check.py`, `findfile_check.py` green (no-property buffers render unchanged; editing still works).
- [ ] `busybox ls` in the frame is colored, no `?[` litter.
- [ ] Final whole-implementation review, then `superpowers:finishing-a-development-branch`.
