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
