# Phase 27 — Deepening the Lisp machine

Phase 25 made the frame a place to *read* (output streams into buffers); Phase
27 makes it a place to *type into a running program* — closing the gaps that
kept the graphical machine tethered to the serial console.

## 27.1 — Interactive stdin in the frame

**The gap.** A program run from the frame (`(run "wc")`) inherited the frame's
own fd 0. But the frame's fd 0 is the **serial console** — the UART nobody is
typing on under a GUI. So anything that *read* stdin blocked forever: `wc`
never saw input, an interactive prompt hung. `frame.l` already had the mirror
image solved (a child's stdout pipes into the buffer via `stream-thunk`); it
was missing the return path.

**The fix — a second pipe.** `stream-thunk` now opens *two* pipes around the
forked child: the existing output pipe (child fd 1 → buffer) and a new input
pipe (frame keyboard → child fd 0). The classic Unix fd dance, doubled:

- child `dup2`s the output write-end onto fd 1 and the input read-end onto fd 0,
  then closes all four raw ends;
- the parent closes the output write-end and the input read-end — it only ever
  *reads* output and *writes* input;
- in the poll loop, a typed `(char N)` event is both **echoed into the buffer**
  (a terminal shows what you type) and **forwarded to the child's stdin** with
  `fd-write`. RET arrives as char 10, so Enter sends a newline.

**EOF and signals.** `C-d` closes the parent's write end, so the child's next
read on fd 0 returns 0 bytes — the Unix end-of-input convention, which is how a
filter like `wc` knows to stop and print. A `sin` flag guards against the
double-close (C-d, then the post-loop cleanup if the child exited first). `C-c`
is unchanged: it group-kills the whole job (the Phase 26.3 process-group fix),
so the program *and* the Lisp wrapper that forked it die together.

**No kernel changes.** Everything rides on primitives that already existed —
`pipe`, `dup2`, `close`, `fd-read`, `fd-write` — so this phase is pure Lisp in
`frame.l`. The discipline was test-first all the same: `tools/stdin_check.py`
boots the frame, runs `wc`, types `helloworld` + RET into it (11 bytes), sends
C-d, and OCRs the buffer to confirm both the echoed input *and* wc's answer,
`11`, appear. Run before the change, it fails exactly as the bug predicts (no
echo, no count — wc is blocked on the serial console). See
`docs/images/phase-27-interactive-stdin.png`.

**Known follow-ups.** Raw byte forwarding means backspace is sent as a literal
char-8 rather than editing the line; cooked/line-editing input and a real
in-buffer comint history are the next refinements. Editable file buffers
(`find-file`, edit, save back to disk) are the larger next step in this arc.

## 27.2 — Cooked line editing and REPL history

**Cooked input.** 27.1 forwarded every keystroke to the child raw, so
backspace travelled down the pipe as a literal char-8 and a program saw
`help\x08lo`. A real terminal runs in *canonical* mode: it buffers the line
locally and only delivers it on RET. `stream-thunk` now keeps a `line`
accumulator — ordinary chars echo and append, **backspace** edits the buffered
line (and erases the echoed glyph) without sending anything, **RET** delivers
`line` + newline, and **C-d** flushes a pending line (no newline) or, on an
empty line, closes the pipe for EOF — exactly the classic tty behavior of
"C-d twice ends input mid-line". `tools/lineedit_check.py` types `help`, a
backspace, then `lo` into a live `wc`; the byte count is `6` (`hello\n`), not
`8`, proving the edit happened before any byte was sent.

**REPL history.** The REPL now remembers submitted forms (most-recent-first)
and walks them with **Up/Down**, which the kernel already cooks into C-p/C-n.
`repl-hist-idx` tracks the shown entry (-1 = the live line); `repl-set-input`
swaps the text after the prompt for a recalled form, ready to re-run or edit.
`tools/history_check.py` evaluates `(+ 1 2)` then `(* 2 5)`, presses Up twice
back to `(+ 1 2)`, and RET re-evaluates it to `3`.

## 27.3 — Editable file buffers

The Emacs-machine step: open a file into a buffer, edit it, save it back to
disk -- all from the keyboard. `(find-file "/path")` splits a window, reads the
file into a fresh buffer (a missing path starts empty), records the buffer's
path in the `file-of` alist, and -- like `(teapot)` -- returns selection to the
REPL so the REPL's own "print the value and re-prompt" lands in the REPL, not
the new buffer (the bug the first cut hit: trailing inserts corrupting the
prompt). Switch into the file with **C-x o**.

For that to work, dispatch had to learn that **not every buffer is the REPL**.
A new C primitive `(current-buffer)` returns the selected window's handle;
`editing-p` compares it to `repl-buf`. In a file buffer the keymap is plain
editing -- RET inserts a newline (it does not evaluate), backspace deletes
before point, characters self-insert -- while **C-b/C-f** (and the Left/Right
arrows, newly cooked to them) move point. **C-x C-s** (`save-buffer`) writes the
buffer back via `creat` + `fd-write`; note C-x C-s is two CTRL events, so the
C-x map now dispatches on the whole event, not a bare char.

`tools/findfile_check.py` drives the full loop: find-file a new path, C-x o,
type `hello edit`, C-x C-s, C-x o back, then `(cat "/n.txt")` -- and the saved
text streams back from disk. See `docs/images/phase-27-editable-buffer.png`.

**Known limitation.** There is no truncate syscall yet, so `save-buffer` writes
from offset 0 without shortening the file -- re-saving to a *shorter* length
leaves stale trailing bytes. New files and same-or-longer saves are correct;
a truncate (or unlink-then-creat) is the follow-up. Vertical (C-p/C-n) motion
in a file buffer is also still TODO -- C-f/C-b traverse newlines, so every
position is reachable, just not by line.

**Follow-up fixes from live use.** C-b/C-f (and the Left/Right arrows) were
first wired only for file buffers; they now also move point within the REPL
input, clamped at the prompt (`repl-start`) so you can't back into the prompt
text, and backspace now keys off `point` (not just "is there input") so it
deletes the char before the cursor and never eats the prompt. And `make
run-gui` no longer passes `zoom-to-fit` to cocoa: with it on, QEMU opened a
small default window and scaled the guest *down* into it (the persistent
"tiny window"); without it the window opens at the scanout's native size.

## 27.4 — Windows scroll to keep point visible

Buffers taller than their window simply ran off the bottom: the prompt and the
latest output scrolled out of sight with no way to follow them. The machinery
was already there -- each window has a `top_line` (Emacs's *window-start*), and
layout renders lines `[top_line, top_line + text_rows)` -- but nothing ever
moved it. The fix, in `layout_leaf`, is the Emacs scroll rule: count point's
line, then nudge `top_line` minimally to bring it back on screen --
`top_line = point_line - text_rows + 1` when point fell below the bottom, or
`= point_line` when it rose above the top. Minimal scrolling means a REPL whose
output grows downward stays pinned to the bottom (point is always the last
line), while moving point up scrolls up by exactly the overflow. KTEST `rd:
scroll follows point` (a 20-line buffer in a ~6-row window scrolls to the end,
then back to 0 when point returns home) plus `tools/scroll_check.py` (50 lines
at the REPL; the banner scrolls off, line 49 and a fresh prompt stay visible).
