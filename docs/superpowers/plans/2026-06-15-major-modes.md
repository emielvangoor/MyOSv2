# Major Modes for the Lisp Machine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** give the graphical Lisp machine a real Emacs-style major-mode system —
`fundamental`/`text`/`repl`/`lisp-interaction` modes, a `*scratch*` default
buffer, `C-j` inline eval, an interactive `C-x C-f`, and a mode line that names
the active mode.

**Architecture:** A pure mode *model* lives in a new `user/lisp/modes.l`
(registry, `define`/derived lookup, buffer-local variables, `key-lookup`) that
loads in the plain serial REPL so it is unit-testable. The mode *definitions*
and the dispatch wiring live in `user/lisp/frame.l`. One small C change in
`src/rd_core.c` adds a per-buffer `mode_line` string painted into the existing
mode line. Being a REPL stops being "in the `repl-bufs` list" and becomes
"`major-mode` is `repl-mode`."

**Tech Stack:** C (AArch64 freestanding kernel + user lib), the in-house Lisp
(`bootstrap.l` → `system.l` → `modes.l` → `frame.l`), `make test` KTESTs
(`src/tests.c`), and QEMU integration checks (`tools/*_check.py`: serial REPL +
QMP keyboard injection + screendump OCR).

**Key conventions you must know:**
- Keymaps are data: a binding is `(kind code command-symbol "description")`,
  e.g. `(list 'ctrl 97 'cmd-beginning-of-line "C-a ...")`. Ctrl bindings key on
  the **letter's** ASCII code (97=`a`, 106=`j`, 112=`p`, 110=`n`, 114=`r`).
- Lisp primitives available: `cons car cdr list nth nthcdr cadr caddr cddr
  assoc filter foreach member length eq equal = > < >= <= + - * cond while let
  lambda defun defmacro setq not null and or progn if mapcar symbol-name
  string-concat string-length string-from-char substring read-string eval
  prin1-to-string`. `assoc` compares keys with `equal` (works for fixnums and
  symbols). Buffer handles are fixnums.
- Frame buffer primitives (C): `make-buffer set-buffer current-buffer insert
  delete-char point goto-char buffer-length char-at buffer-substring echo
  redisplay split-below split-right other-window`.
- Build wiring for `.l` files: `Makefile` var `LISP_FILES`, `src/initrd.c`
  externs + `add_prog`, `user/lm.c` load calls.
- All commits are gated by a pre-commit hook that runs `make test`; the suite
  must be green (currently **155** KTESTs) at every commit.

---

## File Structure

- **`user/lisp/modes.l`** (new) — pure mode model: `*modes*` registry,
  `register-mode`, `mode-desc`/`mode-parent`/`mode-keymap`, `mode-field` +
  field accessors, `key-lookup` (moved here from frame.l), `mode-key-lookup`,
  `*buffer-locals*` + `plist-get`/`plist-put`/`buffer-local-get`/
  `buffer-local-set`, `buffer-mode`. No graphics primitives → loads in the
  serial REPL.
- **`user/lisp/frame.l`** (modified) — mode definitions (`fundamental`/`text`/
  `repl`/`lisp-interaction`), their handler functions, `set-major-mode`,
  `eval-last-sexp`, the rewritten `dispatch`, `repl-here`, the `C-x C-f` /
  `C-x r` bindings, scratch boot, mode-aware `frame-tick` recovery, and
  mode-aware `describe-mode`/`describe-bindings`. `key-lookup` removed (now in
  modes.l).
- **`src/rd.h`** (modified) — `char mode_line[24];` added to `struct rd_buffer`.
- **`src/rd_core.c`** (modified) — init `mode_line` empty in `rd_buf_init`;
  paint `-- name  (mode_line) --` in the `modeline:` block.
- **`user/lm_gfx.c`** (modified) — `(set-mode-line-name str)` primitive +
  registration.
- **`src/tests.c`** (modified) — KTEST `rd: modeline shows mode name`.
- **`Makefile`, `src/initrd.c`, `user/lm.c`** (modified) — load `modes.l`.
- **`tools/modes_check.py`** (new) — serial-REPL unit tests for the model.
- **`tools/scratch_check.py`, `tools/lispinteraction_check.py`,
  `tools/replmode_check.py`** (new) — frame OCR checks.
- **`tools/findfile_check.py`** (rewritten) — interactive `C-x C-f` flow.
- **13 existing frame checks** (modified) — add a one-line "open a REPL here"
  preamble (Task 7).

---

## Task 1: The pure mode model (`modes.l`) + build/load wiring

**Files:**
- Create: `user/lisp/modes.l`
- Modify: `Makefile:33` (`LISP_FILES`), `src/initrd.c:51-53,109-111`,
  `user/lm.c:201-202`
- Modify: `user/lisp/frame.l` (remove `key-lookup`, lines 238-243)
- Test: `tools/modes_check.py` (new)

- [ ] **Step 1: Write the failing serial-REPL test**

Create `tools/modes_check.py`:

```python
#!/usr/bin/env python3
"""
modes_check.py -- unit tests for the pure major-mode model (modes.l), driven
over the plain serial REPL (which loads bootstrap -> system -> modes, but NOT
frame). Each assertion sends a form and checks the printed value.
"""
import sys
sys.path.insert(0, "tools")
from lm_harness import Qemu


def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1

        def check(form, want):
            q.send_line(form)
            ok = q.expect(want.encode(), 5)
            print(("ok  " if ok else "FAIL") + f" {form!r} -> {want!r}")
            return ok

        # Fixtures: a parent mode 'p and a child 'c that inherits its handlers.
        q.send_line("(register-mode 'p (list nil nil 'pins 'pret nil \"P doc\" \"P\"))")
        q.send_line("(register-mode 'c (list 'p nil nil nil nil nil \"C\"))")

        all_ok = True
        all_ok &= check("(mode-parent 'c)", "p")          # derived parent
        all_ok &= check("(mode-return-fn 'c)", "pret")     # inherited handler
        all_ok &= check("(mode-self-insert-fn 'c)", "pins")
        all_ok &= check("(mode-doc 'c)", "P doc")          # inherited doc
        all_ok &= check("(mode-line-name-of 'c)", "C")     # own field wins
        # buffer-local round-trip (handles are arbitrary fixnums here)
        q.send_line("(buffer-local-set 7 'x 42)")
        all_ok &= check("(buffer-local-get 7 'x 0)", "42")
        all_ok &= check("(buffer-local-get 7 'y 99)", "99")  # default
        all_ok &= check("(buffer-local-get 8 'x 0)", "0")    # independent handle
        all_ok &= check("(buffer-mode 8)", "fundamental-mode")  # default mode
        # keymap-chain lookup: a key in the child's keymap is found; absent -> nil
        q.send_line("(register-mode 'k (list nil (list (list 'ctrl 106 'foo \"C-j\")) nil nil nil nil \"K\"))")
        all_ok &= check("(nth 2 (mode-key-lookup 'k '(ctrl 106)))", "foo")
        all_ok &= check("(mode-key-lookup 'k '(ctrl 97))", "nil")

        if all_ok:
            print("PASS: mode model verified")
            return 0
        print("FAIL: a model assertion failed")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 tools/modes_check.py`
Expected: FAIL — `register-mode` is unbound (modes.l does not exist / is not
loaded), so the first `check` lines report `FAIL`.

- [ ] **Step 3: Create `user/lisp/modes.l`**

```lisp
;;; modes.l -- the major-mode MODEL (pure data; loads in the serial REPL too).
;;;
;;; A mode is a symbol with a descriptor stored in *modes*:
;;;   (parent keymap on-self-insert on-return setup-fn doc mode-line-name)
;;; Inherited fields resolve UP the parent chain (mode-field), so a derived
;;; mode only restates what it changes. Buffer-local variables live in
;;; *buffer-locals*, an alist keyed by buffer handle (a fixnum). This file has
;;; NO dependency on the graphics primitives, so the plain `lisp` REPL can load
;;; it and the mode model can be unit-tested without a frame.

;; ---- the mode registry --------------------------------------------------
(setq *modes* nil)            ; alist (mode-symbol . descriptor)

(defun register-mode (sym desc)
  "Register (or replace) mode SYM with descriptor DESC, a 7-element list:
(parent keymap on-self-insert on-return setup-fn doc mode-line-name)."
  (setq *modes* (cons (cons sym desc)
                      (filter (lambda (e) (not (eq (car e) sym))) *modes*)))
  sym)

(defun mode-desc (sym) (cdr (assoc sym *modes*)))
(defun mode-parent (sym) (nth 0 (mode-desc sym)))
(defun mode-keymap (sym) (nth 1 (mode-desc sym)))

(defun mode-field (sym idx)
  "Field IDX (>=2) of mode SYM, walking up the parent chain until non-nil."
  (let ((d (mode-desc sym)))
    (cond ((null d) nil)
          ((nth idx d) (nth idx d))
          (t (mode-field (mode-parent sym) idx)))))

;; Field-index accessors (parent=0 keymap=1 handled above).
(defun mode-self-insert-fn (sym) (mode-field sym 2))
(defun mode-return-fn (sym)      (mode-field sym 3))
(defun mode-setup-fn (sym)       (mode-field sym 4))
(defun mode-doc (sym)            (mode-field sym 5))
(defun mode-line-name-of (sym)   (mode-field sym 6))

;; ---- keymaps (key-lookup MOVED here from frame.l so the serial REPL has it)
(defun key-lookup (ev km)
  "The binding for EV (a (kind code ...) event) in keymap KM, or nil."
  (cond ((null km) nil)
        ((and (eq (nth 0 (car km)) (car ev)) (= (nth 1 (car km)) (cadr ev)))
         (car km))
        (t (key-lookup ev (cdr km)))))

(defun mode-key-lookup (sym ev)
  "Look EV up in SYM's keymap chain (mode, then parents). nil if unbound."
  (cond ((null sym) nil)
        ((key-lookup ev (mode-keymap sym)) (key-lookup ev (mode-keymap sym)))
        (t (mode-key-lookup (mode-parent sym) ev))))

;; ---- buffer-local variables --------------------------------------------
(setq *buffer-locals* nil)    ; alist (handle . plist), plist = (sym val sym val ..)

(defun plist-get (pl sym dflt)
  (cond ((null pl) dflt)
        ((eq (car pl) sym) (cadr pl))
        (t (plist-get (cddr pl) sym dflt))))

(defun plist-put (pl sym val)
  (cond ((null pl) (list sym val))
        ((eq (car pl) sym) (cons sym (cons val (cddr pl))))
        (t (cons (car pl) (cons (cadr pl) (plist-put (cddr pl) sym val))))))

(defun buffer-local-get (buf sym dflt)
  (let ((e (assoc buf *buffer-locals*)))
    (if e (plist-get (cdr e) sym dflt) dflt)))

(defun buffer-local-set (buf sym val)
  (let ((e (assoc buf *buffer-locals*)))
    (setq *buffer-locals*
          (cons (cons buf (plist-put (if e (cdr e) nil) sym val))
                (filter (lambda (x) (not (= (car x) buf))) *buffer-locals*))))
  val)

(defun buffer-mode (buf) (buffer-local-get buf 'major-mode 'fundamental-mode))

(princ "modes.l loaded.")
```

- [ ] **Step 4: Remove `key-lookup` from `frame.l` (now in modes.l)**

Delete these lines from `user/lisp/frame.l` (currently 238-243):

```lisp
(defun key-lookup (ev km)
  "The binding for EV (a (kind code ...) event) in keymap KM, or nil."
  (cond ((null km) nil)
        ((and (eq (nth 0 (car km)) (car ev)) (= (nth 1 (car km)) (cadr ev)))
         (car km))
        (t (key-lookup ev (cdr km)))))
```

(Leave `run-key` directly below it in place — it stays in frame.l.)

- [ ] **Step 5: Wire `modes.l` into the build and loaders**

In `Makefile` line 33, add `modes` between `system` and `frame`:

```makefile
LISP_FILES  := bootstrap system modes frame
```

In `src/initrd.c`, add the extern (after the `system_l` extern, ~line 52) and
the `add_prog` (after the `system.l` add_prog, ~line 110):

```c
extern unsigned char modes_l[];     extern unsigned int modes_l_len;
```
```c
    add_prog("/lib/modes.l",     modes_l,     (uint64_t)modes_l_len);
```

In `user/lm.c`, add a load call right after the `system.l` load (line 202):

```c
    lm_eval_all_str("(load \"/lib/system.l\")");
    lm_eval_all_str("(load \"/lib/modes.l\")");
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `python3 tools/modes_check.py`
Expected: PASS — every `ok` line, then `PASS: mode model verified`.

- [ ] **Step 7: Verify the suite is still green**

Run: `make test`
Expected: `==== self-tests: 155 passed, 0 failed ====`

- [ ] **Step 8: Commit**

```bash
git add user/lisp/modes.l user/lisp/frame.l Makefile src/initrd.c user/lm.c tools/modes_check.py
git commit -m "feat(frame): pure major-mode model (modes.l) + serial unit tests"
```

---

## Task 2: Mode-line field + `set-mode-line-name` primitive

**Files:**
- Modify: `src/rd.h:39-49` (struct), `src/rd_core.c:31-39` (init),
  `src/rd_core.c:321-333` (render)
- Modify: `user/lm_gfx.c` (new primitive + registration ~line 617)
- Test: `src/tests.c` (new KTEST near `test_rd_single_window_layout`, ~line 1372)

- [ ] **Step 1: Write the failing KTEST**

In `src/tests.c`, add after `test_rd_single_window_layout` (~line 1399):

```c
static void test_rd_modeline_mode_name(void)
{
    rd_fresh();
    rd_buf_insert(&rdb, "x");
    // Simulate (set-mode-line-name "Lisp Interaction") on the shown buffer.
    rd_scpy(rdb.mode_line, "Lisp Interaction", (int)sizeof(rdb.mode_line));
    rd_layout(&rdf);
    int ml = rdf.rows - 2;
    // The modeline must contain the mode name in parentheses: "(Lisp ...".
    int found = 0;
    for (int c = 0; c < rdf.cols - 1; c++) {
        if (rd_cell_at(&rdf, c, ml)->ch == '(' &&
            rd_cell_at(&rdf, c + 1, ml)->ch == 'L') { found = 1; }
    }
    KASSERT(found);
}
```

Register it in the test table (the `{ "rd: single window layout", ... }` block,
search for `"rd: single window layout"`); add right after that line:

```c
    { "rd: modeline shows mode name", test_rd_modeline_mode_name },
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make test`
Expected: FAIL to **compile** — `struct rd_buffer` has no `mode_line` member.
(A compile failure is the expected "red" here.)

- [ ] **Step 3: Add the struct field**

In `src/rd.h`, inside `struct rd_buffer` (after `char name[32];`, line 40):

```c
    char name[32];
    char mode_line[24];             // mode name shown in the mode line ("" = none)
```

- [ ] **Step 4: Initialize it in `rd_buf_init`**

In `src/rd_core.c`, `rd_buf_init` (line 31-39), after the `rd_scpy(b->name, ...)`:

```c
    rd_scpy(b->name, name, (int)sizeof(b->name));
    b->mode_line[0] = 0;            // no mode name until set-mode-line-name
```

- [ ] **Step 5: Paint it in the modeline block**

In `src/rd_core.c`, replace the modeline name/space section (lines 326-329):

```c
    const char *nm = b ? b->name : "?";
    for (int i = 0; nm[i] && n < (int)sizeof(ml) - 2; i++) { ml[n++] = nm[i]; }
    ml[n++] = ' ';
    while (n < w->w && n < (int)sizeof(ml) - 1) { ml[n++] = '-'; }
```

with (adds `  (mode_line)` when non-empty):

```c
    const char *nm = b ? b->name : "?";
    for (int i = 0; nm[i] && n < (int)sizeof(ml) - 2; i++) { ml[n++] = nm[i]; }
    ml[n++] = ' ';
    if (b && b->mode_line[0]) {                       // "  (ModeName)"
        ml[n++] = ' ';
        if (n < (int)sizeof(ml) - 1) { ml[n++] = '('; }
        for (int i = 0; b->mode_line[i] && n < (int)sizeof(ml) - 3; i++) {
            ml[n++] = b->mode_line[i];
        }
        if (n < (int)sizeof(ml) - 2) { ml[n++] = ')'; }
        if (n < (int)sizeof(ml) - 1) { ml[n++] = ' '; }
    }
    while (n < w->w && n < (int)sizeof(ml) - 1) { ml[n++] = '-'; }
```

- [ ] **Step 6: Run the KTEST to verify it passes**

Run: `make test`
Expected: PASS, `rd: modeline shows mode name` among them; total now **156**.

- [ ] **Step 7: Add the `set-mode-line-name` Lisp primitive**

In `user/lm_gfx.c`, after `Gcurrent_buffer` (line 113), add:

```c
/* (set-mode-line-name "str") -> set the SELECTED window's buffer's mode-line
 * name (the "(Mode)" shown in the mode line). Lisp's set-major-mode calls it. */
DEFGFX("set-mode-line-name", Gset_mode_line_name, 1, 1) {
    (void)env;
    const char *s = req_string(CAR(args), "set-mode-line-name: expected a string");
    struct rd_buffer *b = cur();
    int i = 0;
    for (; s[i] && i < (int)sizeof(b->mode_line) - 1; i++) { b->mode_line[i] = s[i]; }
    b->mode_line[i] = 0;
    return Qt;
}
```

Register it next to `register_Gcurrent_buffer();` (~line 617):

```c
    register_Gmake_buffer(); register_Gset_buffer(); register_Gcurrent_buffer();
    register_Gset_mode_line_name();
```

- [ ] **Step 8: Rebuild to confirm the primitive compiles**

Run: `make` (then `make test` to confirm still green)
Expected: builds clean; `==== self-tests: 156 passed, 0 failed ====`.

- [ ] **Step 9: Commit**

```bash
git add src/rd.h src/rd_core.c user/lm_gfx.c src/tests.c
git commit -m "feat(rd): per-buffer mode-line name + (set-mode-line-name) primitive"
```

---

## Task 3: Mode definitions + `set-major-mode` + `eval-last-sexp` (additive)

This task only *adds* mode definitions and helpers to `frame.l`. It does **not**
rewire dispatch or boot yet, so the suite stays green. Verified by a temporary
serial check is not possible (these need the frame), so we verify by building
and running an existing frame check unchanged (it must still pass) plus the
unit model from Task 1.

**Files:**
- Modify: `user/lisp/frame.l` (add a "major modes" section after the
  `kill-word` definition, ~line 174, before the `keymaps as DATA` section)

- [ ] **Step 1: Add `edit-lo` rewrite + mode definitions + helpers**

In `user/lisp/frame.l`, find `edit-lo` (lines 125-127):

```lisp
(defun edit-lo ()
  "Leftmost editable position: 0 in a file buffer, the prompt in the REPL."
  (if (editing-p) 0 (cur-start)))
```

Replace it with a buffer-mode form (so it no longer needs `editing-p`):

```lisp
(defun edit-lo ()
  "Leftmost editable position: the prompt in a REPL, else 0."
  (if (eq (buffer-mode (current-buffer)) 'repl-mode) (cur-start) 0))
```

Then add this block immediately before the `;; ---- keymaps as DATA` comment
(~line 176):

```lisp
;; ---- the major modes (the MODEL is in modes.l; definitions live here) ----
;; Each mode names handler functions that touch the buffer (insert, prompt,
;; eval). The descriptor order is
;;   (parent keymap on-self-insert on-return setup-fn doc mode-line-name).
;; Hierarchy (behavior axis): special-mode is the inert root; surface-mode and
;; fundamental-mode are its two children. "Graphics is the substrate" lives on
;; the RENDERING axis (rd_core paints pixels for all buffers), not here.
;;   special-mode
;;     surface-mode          (a pixel canvas; ignores typing)
;;     fundamental-mode      (adds text self-insert / RET)
;;       text / repl / lisp-interaction

(defun ignore-char (ch) ch)              ; inert: a key in the canvas does nothing
(defun ignore-ret () nil)
(register-mode 'special-mode
  (list nil nil 'ignore-char 'ignore-ret nil
        "Special: an inert displayed buffer. Typing does nothing; ctrl/meta keys
fall through to the global keymap (so C-x o etc. still work)."
        "Special"))

(register-mode 'surface-mode
  (list 'special-mode nil nil nil nil
        "Surface: a pixel canvas an external renderer (e.g. the teapot) draws
into. Keys do nothing -- it is not text."
        "Surface"))

(defun fundamental-self-insert (ch) (insert (string-from-char ch)))
(defun fundamental-return () (insert "\n"))
(register-mode 'fundamental-mode
  (list 'special-mode nil 'fundamental-self-insert 'fundamental-return nil
        "Fundamental mode: text self-inserts; RET inserts a newline."
        "Fundamental"))

(register-mode 'text-mode
  (list 'fundamental-mode nil nil nil nil
        "Text mode: plain text editing. C-x C-s saves to the visited file."
        "Text"))

(defun repl-self-insert (ch) (insert (string-from-char ch)))
(defun repl-return () (repl-eval))
(defun repl-setup () (goto-char (buffer-length)) (prompt))
(setq repl-keymap
  (list
    (list 'ctrl 112 'cmd-history-prev "C-p    previous input (history)")
    (list 'ctrl 110 'cmd-history-next "C-n    next input (history)")))
(register-mode 'repl-mode
  (list 'fundamental-mode repl-keymap 'repl-self-insert 'repl-return 'repl-setup
        "REPL mode: RET evaluates the input; C-p/C-n walk the input history."
        "REPL"))

(defun scratch-setup ()
  (insert ";; *scratch* -- type a Lisp form and press C-j to evaluate it.\n\n"))
(setq lisp-interaction-keymap
  (list (list 'ctrl 106 'eval-last-sexp "C-j    eval the form before point")))
(register-mode 'lisp-interaction-mode
  (list 'fundamental-mode lisp-interaction-keymap nil nil 'scratch-setup
        "Lisp Interaction: RET=newline; C-j evaluates the form before point."
        "Lisp Interaction"))

(defun set-major-mode (buf mode-sym)
  "Make BUF use MODE-SYM: record it (buffer-local), select it, paint the mode
line, and run the mode's setup-fn."
  (buffer-local-set buf 'major-mode mode-sym)
  (set-buffer buf)
  (set-mode-line-name (mode-line-name-of mode-sym))
  (let ((s (mode-setup-fn mode-sym))) (if s (eval (list s)) nil))
  mode-sym)

;; ---- lisp-interaction: evaluate the form before point (C-j) ----
(defun sexp-ws-p (c) (or (= c 32) (= c 10) (= c 9)))   ; space/newline/tab

(defun last-sexp-start (p)
  "Scan left from P over one balanced form; return its start position."
  (let ((q (- p 1)) (lo (edit-lo)))
    (while (and (> q lo) (sexp-ws-p (char-at q))) (setq q (- q 1)))  ; trailing ws
    (if (and (>= q 0) (= (char-at q) 41))            ; ')' -> match parens back
        (let ((depth 0) (done nil))
          (while (not done)
            (let ((c (char-at q)))
              (cond ((= c 41) (setq depth (+ depth 1)))
                    ((= c 40) (setq depth (- depth 1))))
              (cond ((and (= c 40) (= depth 0)) (setq done t))
                    ((<= q lo) (setq done t))
                    (t (setq q (- q 1))))))
          q)
      (progn                                          ; an atom: back over it
        (while (and (> q lo)
                    (not (sexp-ws-p (char-at (- q 1))))
                    (not (= (char-at (- q 1)) 40)))
          (setq q (- q 1)))
        q))))

(defun eval-last-sexp ()
  "C-j in lisp-interaction-mode: eval the form before point; insert its value."
  (let ((start (last-sexp-start (point))) (end (point)))
    (let ((src (buffer-substring start end)))
      (goto-char end)
      (insert "\n")
      (let ((val (eval (read-string src))))
        (flush-output)
        (insert (prin1-to-string val))
        (insert "\n")))))
```

- [ ] **Step 2: Build and confirm an unchanged frame check still passes**

Run: `make && python3 tools/lineedit_check.py`
Expected: PASS (boot still uses the old REPL path; these additions are inert
until Task 4 wires them). If it fails, the additions broke load — check for a
typo in the new block.

- [ ] **Step 3: Confirm the KTEST suite is green**

Run: `make test`
Expected: `==== self-tests: 156 passed, 0 failed ====`

- [ ] **Step 4: Commit**

```bash
git add user/lisp/frame.l
git commit -m "feat(frame): define fundamental/text/repl/lisp-interaction modes + eval-last-sexp"
```

---

## Task 4: Rewire `dispatch` onto modes; boot stays a REPL

Make every keystroke route through the current buffer's mode. Drop
`repl-bufs`/`repl-buf-p`/`editing-p`. The boot buffer becomes a **repl-mode**
buffer (not scratch yet) so all existing frame checks stay green (RET still
evaluates in repl-mode).

**Files:**
- Modify: `user/lisp/frame.l` — `dispatch` (256-286), `cmd-history-prev/next`
  (191-192), `frame-setup` (673-684), `cur-start`/`set-cur-start` (26-35),
  `new-repl` (95-104); remove `repl-bufs`/`repl-buf-p`/`editing-p`/`edit-key`.

- [ ] **Step 1: Migrate REPL prompt position to a buffer-local**

Replace `cur-start` and `set-cur-start` (lines 26-35):

```lisp
(defun cur-start ()
  "Where the SELECTED buffer's REPL input begins (0 if it has no prompt yet)."
  (buffer-local-get (current-buffer) 'repl-start 0))

(defun set-cur-start (pos)
  "Record the current buffer's input start."
  (buffer-local-set (current-buffer) 'repl-start pos))
```

Delete the now-unused `repl-starts` state and `in-list`/`repl-buf-p`/`editing-p`
(lines 16-24 and 89-93). Specifically remove:

```lisp
(setq repl-starts nil)
```
```lisp
(defun in-list (x lst)                 ; numeric membership (handles are fixnums)
  (cond ((null lst) nil) ((= x (car lst)) t) (t (in-list x (cdr lst)))))

(defun repl-buf-p (b) (in-list b repl-bufs))
```
```lisp
(defun editing-p ()
  "t when the selected buffer is a plain editable buffer, not a REPL."
  (not (repl-buf-p (current-buffer))))
```

Also delete `(setq repl-bufs nil)` (line 16) and the `edit-key` function
(lines 114-119).

- [ ] **Step 2: Drop the `editing-p` guards in history commands**

Replace `cmd-history-prev`/`cmd-history-next` (lines 191-192):

```lisp
(defun cmd-history-prev () (repl-history-prev))
(defun cmd-history-next () (repl-history-next))
```

(They are now only reachable via `repl-keymap`, so the guard is redundant.)

- [ ] **Step 3: Rewrite `dispatch` to route through the mode**

Replace `dispatch` (lines 256-286) with:

```lisp
(defun run-mode-or-global (ev)
  "Run EV via the current mode's keymap chain, else the global keymap."
  (let ((b (mode-key-lookup (buffer-mode (current-buffer)) ev)))
    (if b (progn (eval (list (nth 2 b))) t)
      (if (run-key ev global-keymap) t (progn (echo "unbound key") nil)))))

(defun mode-backspace (m)
  "Backspace in mode M: in a REPL never past the prompt; else one char."
  (if (eq m 'repl-mode)
      (if (> (point) (cur-start)) (delete-char 1) nil)
    (if (> (point) 0) (delete-char 1) nil)))

(defun dispatch (ev)
  "Route one cooked input event through the selected buffer's major mode."
  (cond
    ((null ev) nil)
    (mb-action (mb-dispatch ev))                          ; minibuffer owns input
    (pending-cx (handle-cx ev))                           ; C-x <key>
    ((eq (car ev) 'mouse)
     (select-window-at (cadr ev) (caddr ev)))
    ((eq (car ev) 'meta)
     (if (= (cadr ev) 120) (mb-start 'mx)                 ; M-x
       (run-mode-or-global ev)))
    (pending-ch
     (setq pending-ch nil)
     (if (run-key ev help-keymap) nil (echo "unbound C-h key")))
    ((eq (car ev) 'ctrl)
     (cond ((= (cadr ev) 120) (setq pending-cx t))        ; C-x: await suffix
           ((= (cadr ev) 104) (setq pending-ch t))        ; C-h: await suffix
           ((= (cadr ev) 103) (setq pending-cx nil)       ; C-g: quit
                              (echo "quit"))
           (t (run-mode-or-global ev))))                  ; mode keymap then global
    ((eq (car ev) 'char)
     (let ((m (buffer-mode (current-buffer))) (ch (cadr ev)))
       (cond
         ((= ch 10) (eval (list (mode-return-fn m))))     ; RET: mode decides
         ((= ch 8) (mode-backspace m))                    ; backspace
         (t (eval (list (mode-self-insert-fn m) ch))))))  ; printable: mode decides
    (t nil)))
```

- [ ] **Step 4: Rewrite `new-repl` to set the mode**

Replace `new-repl` (lines 95-104):

```lisp
(defun new-repl ()
  "Turn the selected window's buffer into a FRESH REPL buffer in repl-mode."
  (let ((b (make-buffer "*repl*")))
    (set-major-mode b 'repl-mode)                         ; prompts via repl-setup
    b))
```

- [ ] **Step 5: Rewrite `frame-setup` to use repl-mode (still a REPL for now)**

Replace `frame-setup` (lines 673-684):

```lisp
(defun frame-setup ()
  "Acquire the screen and greet. Returns nil when there is no display."
  (if (frame-init)
      (progn
        (insert "MyOSv2 Graphical Lisp Machine\n")
        (insert "C-x 2/3 split  C-x 0 delete  C-x o next  mouse-1 select\n\n")
        (set-major-mode (current-buffer) 'repl-mode)       ; prompts at end
        (echo "ready")
        (redisplay)
        t)
    (progn (princ "frame: no display\n") nil)))
```

- [ ] **Step 6: Build and run the regression frame checks**

Run: `make && python3 tools/lineedit_check.py && python3 tools/history_check.py && python3 tools/replmotion_check.py && python3 tools/split_check.py`
Expected: all PASS — dispatch is now mode-driven but the frame still boots a
REPL, so behavior is unchanged.

- [ ] **Step 7: Confirm the KTEST suite is green**

Run: `make test`
Expected: `==== self-tests: 156 passed, 0 failed ====`

- [ ] **Step 8: Commit**

```bash
git add user/lisp/frame.l
git commit -m "refactor(frame): route dispatch through major mode; drop repl-bufs/editing-p"
```

---

## Task 5: Boot into `*scratch*`; add `repl-here` / `C-x r`; mode-aware recovery

**Files:**
- Modify: `user/lisp/frame.l` — `frame-setup`, `frame-tick` recovery,
  `cx-keymap` (add C-x r), add `repl-here`
- Test: `tools/scratch_check.py`, `tools/lispinteraction_check.py`,
  `tools/replmode_check.py` (new)

- [ ] **Step 1: Write the failing scratch + lisp-interaction frame checks**

Create `tools/scratch_check.py`:

```python
#!/usr/bin/env python3
"""scratch_check.py -- the frame boots into *scratch* (lisp-interaction-mode)."""
import os, sys, tempfile, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp_screendump
from frame_check import load_font, read_ppm, row_text


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-scratch-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(h // 16)]
        for i, t in enumerate(lines[:6]):
            print(f"  row {i}: {t!r}")
        # The mode line names the mode; the scratch banner is present.
        ml = "".join(lines)
        ok_mode = "(Lisp Interaction)" in ml
        ok_banner = any("scratch" in t for t in lines)
        if ok_mode and ok_banner:
            print("PASS: boots into *scratch* (Lisp Interaction)")
            return 0
        print(f"FAIL: mode={ok_mode} banner={ok_banner}")
        return 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
```

Create `tools/lispinteraction_check.py`:

```python
#!/usr/bin/env python3
"""lispinteraction_check.py -- C-j evaluates the form before point in *scratch*."""
import os, sys, tempfile, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-li-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        qmp_type("(+ 1 2)"); time.sleep(0.4)
        ctrl("j"); time.sleep(0.8)               # C-j: eval the form before point
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(h // 16)]
        for i, t in enumerate(lines[:8]):
            print(f"  row {i}: {t!r}")
        # The result 3 appears on its own line just after the form.
        ok = any(t.strip() == "3" for t in lines)
        print("PASS: C-j evaluated (+ 1 2) -> 3" if ok else "FAIL: result 3 not seen")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run them to verify they fail**

Run: `python3 tools/scratch_check.py`
Expected: FAIL — the frame still boots a `*repl*`, so no `(Lisp Interaction)`
mode line and no scratch banner.

- [ ] **Step 3: Boot into `*scratch*`**

Replace `frame-setup` (the version from Task 4) with:

```lisp
(defun frame-setup ()
  "Acquire the screen, then open *scratch* in lisp-interaction-mode (Emacs-style)."
  (if (frame-init)
      (progn
        (let ((sb (make-buffer "*scratch*")))
          (set-major-mode sb 'lisp-interaction-mode))     ; selects + banner
        (echo "ready")
        (redisplay)
        t)
    (progn (princ "frame: no display\n") nil)))
```

- [ ] **Step 4: Make `frame-tick` error-recovery mode-aware**

In `frame-tick` (lines 707-714), replace the recovery `progn`:

```lisp
        (progn (insert pending) (insert "\n") (prompt) (redisplay))
```

with (only repl-mode re-prompts):

```lisp
        (progn (insert pending) (insert "\n")
               (if (eq (buffer-mode (current-buffer)) 'repl-mode) (prompt) nil)
               (redisplay))
```

- [ ] **Step 5: Add `repl-here` and bind `C-x r`**

Add `repl-here` near `new-repl`:

```lisp
(defun repl-here ()
  "C-x r: turn the current window's buffer into a fresh REPL (clear + prompt)."
  (goto-char (buffer-length)) (delete-char (buffer-length))   ; clear the buffer
  (set-major-mode (current-buffer) 'repl-mode))
```

In `cx-keymap` (the `(setq cx-keymap (list ...))` around line 220), add a row
(before the closing paren of the list):

```lisp
    (list 'char 114 'repl-here       "C-x r    open a REPL in this window")
```

- [ ] **Step 6: Run the two new checks to verify they pass**

Run: `make && python3 tools/scratch_check.py && python3 tools/lispinteraction_check.py`
Expected: both PASS.

- [ ] **Step 7: Add and run a repl-mode check**

Create `tools/replmode_check.py`:

```python
#!/usr/bin/env python3
"""replmode_check.py -- C-x r opens a repl-mode window whose RET evaluates."""
import os, sys, tempfile, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-replmode-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)   # C-x r
        qmp_type("(+ 2 3)\n"); time.sleep(0.8)                        # RET evaluates
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(h // 16)]
        for i, t in enumerate(lines[:8]):
            print(f"  row {i}: {t!r}")
        ml = "".join(lines)
        ok = "(REPL)" in ml and any(t.strip() == "5" for t in lines)
        print("PASS: C-x r REPL evaluates (RET)" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
```

Run: `python3 tools/replmode_check.py`
Expected: PASS — mode line shows `(REPL)` and `5` appears.

- [ ] **Step 8: Confirm the KTEST suite is green**

Run: `make test`
Expected: `==== self-tests: 156 passed, 0 failed ====`

- [ ] **Step 9: Commit**

```bash
git add user/lisp/frame.l tools/scratch_check.py tools/lispinteraction_check.py tools/replmode_check.py
git commit -m "feat(frame): boot into *scratch* (lisp-interaction); C-x r repl-here; mode-aware recovery"
```

---

## Task 6: `surface-mode` — bring graphical buffers into the mode system

Surface buffers (the teapot) keep their shm-canvas mechanics; this only wraps
their creation so they enter `surface-mode` (defined in Task 3). Closes the
"typing into a picture" gap the new dispatch would otherwise open.

**Files:**
- Modify: `user/lisp/frame.l` — add `make-surface` wrapper; `teapot` uses it
- Test: `tools/surface_check.py` (modify — assert `(Surface)` mode line)

- [ ] **Step 1: Update `tools/surface_check.py` to assert the mode line**

Open `tools/surface_check.py`. After the frame loads it currently drives a
surface demo; add the scratch-boot REPL preamble and a `(Surface)` mode-line
assertion. Ensure it imports `qmp, qmp_type, qmp_screendump` from `lm_harness`
and `load_font, read_ppm, row_text` from `frame_check`, and has a `ctrl` helper:

```python
def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})
```

Replace the body after `frame.l loaded` with the teapot-via-REPL flow:

```python
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)   # C-x r: a REPL
        qmp_type("(teapot)\n"); time.sleep(2.5)                       # open *teapot*
        dump = os.path.join(tempfile.gettempdir(), "myosv2-surface-check.ppm")
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(load_font(), w, data, r) for r in range(h // 16)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}")
        ok = any("(Surface)" in t for t in lines)
        print("PASS: *teapot* is a surface-mode buffer" if ok
              else "FAIL: (Surface) mode line not seen")
        return 0 if ok else 1
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 tools/surface_check.py`
Expected: FAIL — `*teapot*`'s mode line shows only `-- *teapot* --` (no mode set
yet), so `(Surface)` is absent.

- [ ] **Step 3: Add the `make-surface` wrapper**

In `user/lisp/frame.l`, add near the surface/teapot code (just above `teapot`,
~line 690):

```lisp
(defun make-surface (name w h)
  "Make a surface (pixel-canvas) buffer in surface-mode. The Lisp face of the
make-surface-buffer C primitive: it allocates the shm canvas; we put the buffer
in surface-mode so keys are inert and the mode line reads (Surface)."
  (let ((b (make-surface-buffer name w h)))
    (if b (set-major-mode b 'surface-mode) nil)
    b))
```

- [ ] **Step 4: Make `teapot` use the wrapper**

In `teapot` (lines 690-699), replace the `make-surface-buffer` call:

```lisp
  (let ((sb (make-surface-buffer "*teapot*" 480 360)))
```

with:

```lisp
  (let ((sb (make-surface "*teapot*" 480 360)))
```

(`set-major-mode` inside `make-surface` selects `sb`; the following
`(set-buffer sb)` in `teapot` is now redundant but harmless — leave it.)

- [ ] **Step 5: Run the check to verify it passes**

Run: `make && python3 tools/surface_check.py`
Expected: PASS — `(Surface)` appears in the `*teapot*` window's mode line.

- [ ] **Step 6: Confirm the KTEST suite is green**

Run: `make test`
Expected: `==== self-tests: 156 passed, 0 failed ====`

- [ ] **Step 7: Commit**

```bash
git add user/lisp/frame.l tools/surface_check.py
git commit -m "feat(frame): surface-mode for graphical buffers; teapot via make-surface wrapper"
```

---

## Task 7: Interactive `C-x C-f`; `text-mode` files; rewrite findfile_check

**Files:**
- Modify: `user/lisp/frame.l` — `find-file`/`save-buffer` (288-323), minibuffer
  (`mb-source` 347-348, `mb-render` 382-398, `mb-commit` 497-509), `cx-keymap`
- Rewrite: `tools/findfile_check.py`
- Test: `tools/textmode_check.py` (new)

- [ ] **Step 1: Write the failing textmode check**

Create `tools/textmode_check.py`:

```python
#!/usr/bin/env python3
"""
textmode_check.py -- C-x C-f opens a file in text-mode in the current window;
edit + C-x C-s saves; the mode line reads (Text). Uses the persistent /disk so
a re-open in a fresh boot would find it; here we verify within one boot by
reading it back via the serial REPL helper (cat).
"""
import os, sys, tempfile, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-textmode-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); ctrl("f"); time.sleep(0.5)   # C-x C-f
        qmp_type("/disk/note.txt\n"); time.sleep(0.8)            # path in minibuffer
        qmp_type("hello text mode"); time.sleep(0.6)            # edit in text-mode
        ctrl("x"); time.sleep(0.2); ctrl("s"); time.sleep(0.8)   # C-x C-s save
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(h // 16)]
        for i, t in enumerate(lines[:8]):
            print(f"  row {i}: {t!r}")
        ml = "".join(lines)
        ok_mode = "(Text)" in ml
        ok_text = any("hello text mode" in t for t in lines)
        print("PASS: C-x C-f text-mode edit + save" if (ok_mode and ok_text)
              else f"FAIL: mode={ok_mode} text={ok_text}")
        return 0 if (ok_mode and ok_text) else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 tools/textmode_check.py`
Expected: FAIL — `C-x C-f` is unbound, so nothing opens; no `(Text)` mode line.

- [ ] **Step 3: Rewrite `find-file`/`save-buffer` over buffer-locals + text-mode**

Replace `find-file` and `save-buffer` (lines 290-323):

```lisp
(defun find-file-here (path)
  "Open PATH in text-mode in the CURRENT window. A missing file starts empty.
The buffer remembers PATH (buffer-local file-path) so C-x C-s knows where to
write."
  (let ((b (make-buffer path)))
    (set-major-mode b 'text-mode)                     ; selects b, mode line (Text)
    (buffer-local-set b 'file-path path)
    (let ((fd (open path)))
      (if (>= fd 0)
          (progn
            (let ((chunk (fd-read fd 1024)))
              (while chunk (insert chunk) (setq chunk (fd-read fd 1024))))
            (close fd))
        nil))                                         ; nonexistent -> empty buffer
    (goto-char 0)
    (redisplay)
    b))

(defun cmd-find-file () (mb-start 'find-file))         ; C-x C-f

(defun save-buffer ()
  "Write the current buffer back to the file it is visiting (C-x C-s)."
  (let ((path (buffer-local-get (current-buffer) 'file-path nil)))
    (if path
        (let ((fd (creat path)))
          (if (>= fd 0)
              (progn (fd-write fd (buffer-substring 0 (buffer-length)))
                     (close fd)
                     (echo (string-concat "saved " path)))
            (echo "save failed")))
      (echo "no file for this buffer"))))
```

- [ ] **Step 4: Teach the minibuffer the free-text `'find-file` action**

In `mb-source` (lines 347-348), return nil for find-file (no candidate list):

```lisp
(defun mb-source ()
  (cond ((eq mb-action 'switch) (buffer-list))
        ((eq mb-action 'find-file) nil)               ; free-text path entry
        (t (command-symbols))))
```

In `mb-render` (line 388-390), add the prompt label. Replace:

```lisp
              (cond ((eq mb-action 'mx) "M-x ")
                    ((eq mb-action 'switch) "Buffer: ")
                    (t "Describe: "))
```

with:

```lisp
              (cond ((eq mb-action 'mx) "M-x ")
                    ((eq mb-action 'switch) "Buffer: ")
                    ((eq mb-action 'find-file) "Find file: ")
                    (t "Describe: "))
```

In `mb-commit` (lines 497-509), add a `'find-file` branch. Replace the `cond`'s
final clauses so it reads:

```lisp
(defun mb-commit ()
  (let ((sel (if mb-matches (nth mb-sel mb-matches) nil))
        (action mb-action)
        (input mb-input))
    (mb-stop)
    (cond
      ((eq action 'switch)
       (if sel (progn (set-buffer (car sel)) (redisplay)) (echo "no such buffer")))
      ((eq action 'find-file)                          ; the typed text IS the path
       (find-file-here input))
      ((eq action 'mx)
       (let ((sym (if sel sel (read-string input))))
         (let ((val (eval (list sym))))
           (flush-output) (insert (prin1-to-string val)) (insert "\n") (prompt))))
      ((eq action 'describe)
       (show-help (describe-function-text (if sel sel (read-string input))))))))
```

(Note: `mb-stop` clears `mb-input`, so we capture it as `input` first.)

- [ ] **Step 5: Bind `C-x C-f`**

In `cx-keymap` (line ~220-229), add a row (C-x C-f is `(ctrl 102)`, `f`=102):

```lisp
    (list 'ctrl 102 'cmd-find-file    "C-x C-f  find file")
```

- [ ] **Step 6: Run the textmode check to verify it passes**

Run: `make && python3 tools/textmode_check.py`
Expected: PASS — `(Text)` mode line and `hello text mode` on screen.

- [ ] **Step 7: Rewrite `tools/findfile_check.py` for the interactive flow**

Replace `tools/findfile_check.py` entirely:

```python
#!/usr/bin/env python3
"""
findfile_check.py -- interactive C-x C-f (Phase 27 modes update).

C-x C-f prompts for a path, opens it in text-mode in the CURRENT window; we
type text, C-x C-s to save, then read it back from disk via a fresh REPL
window (C-x r) and (cat ...). The saved text must stream into the REPL.
"""
import os, sys, tempfile, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text


def qmp_ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-findfile-check.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 30):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        qmp_ctrl("x"); time.sleep(0.2); qmp_ctrl("f"); time.sleep(0.5)  # C-x C-f
        qmp_type("/n.txt\n"); time.sleep(0.8)         # path -> opens in this window
        qmp_type("hello edit\n"); time.sleep(0.6)     # text-mode: real text entry
        qmp_ctrl("x"); time.sleep(0.2); qmp_ctrl("s"); time.sleep(0.8)  # save
        qmp_ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)  # C-x r: REPL
        qmp_type('(cat "/n.txt")\n'); time.sleep(1.0)

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r) for r in range(16)]
        for i, t in enumerate(lines):
            print(f"  row {i}: {t!r}")
        ok = any("hello edit" in t for t in lines)
        print("PASS: C-x C-f edit + save persisted (read back)" if ok
              else "FAIL: saved text did not read back")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
```

Run: `python3 tools/findfile_check.py`
Expected: PASS. If a fixed row index is involved, the script prints every row —
adjust the assertion to the row that shows `hello edit`.

- [ ] **Step 8: Confirm the KTEST suite is green**

Run: `make test`
Expected: `==== self-tests: 156 passed, 0 failed ====`

- [ ] **Step 9: Commit**

```bash
git add user/lisp/frame.l tools/findfile_check.py tools/textmode_check.py
git commit -m "feat(frame): interactive C-x C-f opens files in text-mode (current window)"
```

---

## Task 8: Self-documenting help follows the mode; update remaining frame checks

**Files:**
- Modify: `user/lisp/frame.l` — `describe-mode` (467-475), `describe-bindings`
  (457-465)
- Modify: the frame checks that type a form+RET at boot (now scratch): add a
  one-line "open a REPL here" preamble.

The affected checks (they type a Lisp form expecting RET to evaluate, which now
only happens in repl-mode): `bufswitch_check.py`, `frame_check.py`,
`history_check.py`, `keyedit_check.py`, `lineedit_check.py`, `scroll_check.py`,
`seat_check.py`, `split_check.py`, `stdin_check.py`, `teapot_check.py`.
(`surface_check.py` and `findfile_check.py` were already updated in Tasks 6 and
7. `mx_check.py`, `mxscroll_check.py`, `replmotion_check.py`,
`bufferlist_check.py`, `helpkeys_check.py`, `autostart_check.py` do not type a
form+RET, but verify each still passes — if any relies on the boot buffer being
a REPL, add the same preamble.)

- [ ] **Step 1: Make `describe-mode` read the current mode**

Replace `describe-mode` (lines 467-475):

```lisp
(defun describe-mode ()
  "C-h m: describe the selected buffer's mode (its doc + the keys it adds)."
  (let ((m (buffer-mode (current-buffer))))
    (show-help (string-concat
      "=== " (mode-line-name-of m) " mode ===\n\n"
      (mode-doc m) "\n\n"
      "Mode keys:\n\n" (fmt-bindings (mode-keymap m))
      "\nGlobal keys:\n\n"
      (fmt-bindings global-keymap)
      (fmt-bindings cx-keymap)))))
```

(`fmt-bindings` on a nil keymap returns `""` — safe for modes with no own keys.)

- [ ] **Step 2: Make `describe-bindings` include the current mode's keys**

Replace `describe-bindings` (lines 457-465):

```lisp
(defun describe-bindings ()
  "C-h b: every key binding (current mode's keymap + the global maps)."
  (show-help (string-concat
    "=== Key bindings ===\n\n"
    "-- " (mode-line-name-of (buffer-mode (current-buffer))) " mode --\n"
    (fmt-bindings (mode-keymap (buffer-mode (current-buffer))))
    "\n-- global --\n"
    (fmt-bindings global-keymap)
    (fmt-bindings cx-keymap)
    (fmt-bindings help-keymap)
    "\nC-x      prefix\nC-h      prefix (help)\nC-g      quit\n"
    "M-x      run a command by name\nRET      mode-defined (eval / newline)\n")))
```

- [ ] **Step 3: Verify the help checks still pass**

Run: `make && python3 tools/helpkeys_check.py`
Expected: PASS (C-h b / C-h k / C-h m still render from the live keymaps).

- [ ] **Step 4: Add the REPL preamble to each form-typing frame check**

For each file listed above, find the block right after the frame loads:

```python
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
```

Insert immediately after it, using that file's existing ctrl helper (named
`ctrl` in most, `qmp_ctrl` in `findfile`-style ones — match the file):

```python
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)  # C-x r: REPL in this window
```

If a file has no `ctrl`/`qmp_ctrl` helper, copy this one above `main`:

```python
def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})
```

(and ensure `qmp, qmp_type` are imported from `lm_harness`).

- [ ] **Step 5: Run the full frame-check suite and fix row offsets**

Run each updated check, e.g.:
`for c in bufswitch frame history keyedit lineedit scroll seat split stdin surface teapot mx mxscroll replmotion bufferlist helpkeys autostart; do echo "== $c =="; python3 tools/${c}_check.py || echo "FAILED: $c"; done`

Expected: all PASS. If a check fails on a fixed row index (the screen now shows
a fresh `lisp> ` prompt at the top of the single window after `C-x r`), each
check prints every row — shift the asserted row index to the row that shows the
expected content. Make the minimal index change; do not alter what is asserted.

- [ ] **Step 6: Confirm the KTEST suite is green**

Run: `make test`
Expected: `==== self-tests: 156 passed, 0 failed ====`

- [ ] **Step 7: Commit**

```bash
git add user/lisp/frame.l tools/*_check.py
git commit -m "feat(frame): self-documenting describe-mode/bindings follow the mode; update frame checks for scratch boot"
```

---

## Task 9: Docs — README + roadmap

**Files:**
- Modify: `README.md` (capability summary), `docs/` roadmap file (the Phase 27
  / Lisp-machine section — find it with `grep -rl "Phase 2" docs README.md`)

- [ ] **Step 1: Update the README capability summary**

In `README.md`, in the section describing the graphical Lisp machine, add a
sentence to the effect of:

```markdown
- **Major modes** — the frame boots into `*scratch*` (lisp-interaction-mode:
  `C-j` evaluates the form before point and inserts the result). `C-x C-f`
  opens a file in `text-mode` in the current window; `C-x C-s` saves; `C-x r`
  opens a REPL (repl-mode) in the current window. The mode line names the
  active mode, and `C-h m` / `C-h b` describe it from the live keymaps.
```

- [ ] **Step 2: Update the roadmap**

In the roadmap doc (located in Step 0 via grep), mark the major-mode work done
and reference the spec/plan, e.g.:

```markdown
- [x] Emacs-style major modes (fundamental/text/repl/lisp-interaction),
      `*scratch*` default, interactive `C-x C-f`, mode line names the mode.
      Spec: docs/superpowers/specs/2026-06-15-major-modes-design.md
```

- [ ] **Step 3: Confirm the suite is green and commit**

Run: `make test`
Expected: `==== self-tests: 156 passed, 0 failed ====`

```bash
git add README.md docs
git commit -m "docs: README + roadmap for Emacs-style major modes"
```

---

## Self-Review notes

- **Spec coverage:** §1 modes.l split → T1. §2 mode object / §3 hierarchy
  (special→surface/fundamental→text/repl/lisp-interaction) → T3 (defs) + T1
  (model). §4 buffer-locals → T1 + migration T4. §5 enter-mode
  (`set-major-mode`) → T3. §6 dispatch rewrite → T4. §7 eval-last-sexp → T3.
  §8 mode line (C) → T2. §9 `*scratch*` boot + mode-aware recovery + `repl-here`
  → T5. §9a surface-mode + `make-surface` wrapper + teapot → T6. §10 interactive
  `C-x C-f` → T7. §11 self-documenting help → T8.
  Testing: serial unit (T1 `modes_check`) + frame OCR (`scratch`/
  `lispinteraction`/`replmode`/`textmode`/`surface` + rewritten `findfile`) +
  KTEST (`rd: modeline shows mode name`). Existing-check fallout handled
  explicitly in T8. README/roadmap → T9.
- **Type/name consistency:** descriptor field order `(parent keymap
  on-self-insert on-return setup-fn doc mode-line-name)` is fixed across T1/T3;
  accessors `mode-self-insert-fn`/`mode-return-fn`/`mode-setup-fn`/`mode-doc`/
  `mode-line-name-of` used consistently in T3/T4/T5/T7. `buffer-local-get/set`,
  `buffer-mode`, `set-major-mode`, `find-file-here`, `repl-here`,
  `cmd-find-file`, `set-mode-line-name`, `make-surface`, `ignore-char`,
  `ignore-ret` names match every use. Mode tree: `special-mode` (root) →
  `surface-mode` + `fundamental-mode` → `text`/`repl`/`lisp-interaction`. Ctrl
  codes: C-j=106, C-p=112, C-n=110, C-x C-f=102, C-x r=114 (`r` as a `'char`).
- **KTEST count:** 155 → 156 after T2's `rd: modeline shows mode name`.
```
