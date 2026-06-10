# Phase 24 — The Lisp machine

The goal of Phase 24 is to make a Lisp the **primary userland** of MyOSv2, in the
spirit of Emacs and the Symbolics/LMI Lisp machines, but on the pragmatic
"Emacs-on-Unix" model: the C kernel is untouched, and a Lisp image runs at EL0 as
an ordinary MMU-protected process talking to the kernel through the existing
syscalls. Design spec: `docs/superpowers/specs/2026-06-09-lisp-machine-design.md`.

It is ported from the standalone host interpreter at `~/Code/Sides/lm-lisp`.

## The shared-core architecture (why the language lives in `src/`)

The reader, evaluator, printer, garbage collector and primitive set live in one
platform-neutral file, **`src/lm_core.c`** (+ `src/lm.h`). It never calls libc and
never includes a kernel header. Everything it needs from the outside world is a
handful of hooks:

- `lm_alloc` / `lm_free` — allocation
- `lm_sys_read` / `lm_sys_write` / `lm_open` / `lm_close` — I/O
- `lm_setjmp` / `lm_longjmp` (`src/lm_jmp.S`) — non-local exit for error recovery
- `lm_abort` — unrecoverable error

That core is compiled into **two separate binaries**:

| Build  | Platform layer        | Allocation | I/O            | Extra                |
|--------|-----------------------|------------|----------------|----------------------|
| kernel | `src/lm_platform.c`   | `kmalloc`  | UART / stubbed | KTEST cases drive it |
| user   | `user/lm.c`           | `malloc`   | syscalls       | the REPL + `main`    |

Because they are separate link units there is no symbol clash. The payoff: the
in-kernel KTEST suite (`make test`, gated by the pre-commit hook) red-greens the
*language itself* — arithmetic, lists, conditionals, `let`/`setq`, recursion,
closures, macros, `mapcar`/`apply`, strings, GC and error recovery — on real
hardware, the same way every other subsystem is tested here.

## Conservative garbage collection

The host interpreter had a known bug: the collector never scanned C-stack roots,
so it dodged the problem with a huge threshold and only collecting between REPL
turns. `init` runs forever, so the collector has to be safe to run *mid-eval*.

The fix is **conservative stack scanning**. `user/lm.c` records the high end of
the C stack (`lm_stack_base`) at startup. On collection, `lm_setjmp` spills the
callee-saved registers into a buffer (so no live pointer can hide in a register),
and the collector scans that buffer plus every aligned word from the current SP
up to `lm_stack_base`. Any word that, with its tag bits masked off, matches the
start of a live heap object is treated as a root — validated against the object
list and the tracked `lo`/`hi` address bounds, so a stray integer can never drive
the collector to dereference garbage. False positives merely retain an object;
they never free a live one. With roots now safe, `gc_alloc` collects on demand
when it crosses the threshold.

A `type` byte was added to the GC header so the sweep phase can free a string's
owned character data (the host version leaked it).

## 24.1 — the core port + serial REPL

- `src/lm.h`, `src/lm_core.c`, `src/lm_jmp.S`, `src/lm_platform.c` — the core.
- `user/lm.c` — `/bin/lisp`: the user platform layer + an interactive REPL on the
  serial console (echo + line editing, Ctrl-C raises a flag the core acts on at a
  safe point, so a runaway form can be interrupted without killing the process).
- `user/lisp/bootstrap.l` — the standard library, embedded into the kernel
  (`build/lisp_blob.c`) and unpacked by the initrd to `/lib/bootstrap.l`, which
  `/bin/lisp` `load`s at startup.
- 11 KTEST cases in `src/tests.c`.

Verified under QEMU: `(fact 6)` → `720`, `(mapcar (lambda (x) (* x x)) (range 5))`
→ `(0 1 4 9 16)`, an unbound-variable error is caught and the REPL keeps going.

### Console caveat

Pasting a long line into the serial console can overflow the 16-byte PL011 RX
FIFO faster than the line-reader drains it, dropping characters (including the
terminating newline, which then stalls the read). This is a pre-existing console
limitation, not a Lisp one — human-speed typing is fine, and the Emacs-over-TCP
path (24.1b) bypasses the tty entirely. When *scripting* the console (the
integration checks in `tools/` do), send input char-by-char with ~12 ms gaps:

```python
for ch in "lisp -serve\r":
    qemu_stdin.write(ch.encode()); qemu_stdin.flush()
    time.sleep(0.012)
```

## 24.1b — the network REPL (Doom Emacs ↔ the live image)

`lisp -serve [port]` (default 7777) turns `/bin/lisp` into a TCP REPL server:
`socket(SOCK_STREAM)` → `bind` → `listen` → blocking `accept`. On each
connection the core's current streams (`lm_cur_in`/`lm_cur_out`) are pointed at
the socket (`tty=0`: no echo or line editing — the editor at the far end does
that) and the same `lm_repl_step()` loop runs until the peer disconnects. Then
the server loops back into `accept`. **The image persists across connections**
— symbol table and global bindings live in the server process and are untouched
between visits — which is the whole Lisp-machine point: you accrete a living
system, you don't restart it.

Design decisions, and why:

- **A dedicated blocking server, not a tty+socket multiplexer.** The kernel's
  `poll()` treats the console as always-ready, so a poll loop over both would
  busy-spin. Blocking `accept`/`read` sleep in the kernel and are woken by
  interrupts (the house rule: interrupts over polling). Ctrl-C on the console
  interrupts a blocked `accept` (the kernel returns the EINTR convention, <0),
  which is how the server is stopped.
- **Errors travel over the socket.** `lm_error` now reports through
  `lm_cur_out` (falling back to raw fd 2 only when no stream is set up, e.g.
  while loading the bootstrap). A remote user must see their typo in their
  buffer; a message on the guest serial console would be invisible to them.
  This is KTEST-covered (`lm: errors go to lm_cur_out`).
- **Port 7777, not the classic 7000.** macOS's AirPlay Receiver (ControlCenter)
  listens on 7000, so a `hostfwd` on 7000 makes QEMU fail to start on a Mac.
  `make run` forwards host:7777 → guest:7777.

### Emacs glue (`user/lisp/lm-mode.el`)

The host half: a comint buffer over `make-network-process`. In Doom Emacs:

```elisp
;; config.el
(load! "~/Code/Sides/os/user/lisp/lm-mode.el")
(add-hook 'lisp-mode-hook #'lm-minor-mode)
```

Then `make run` → type `lisp -serve` at the guest shell → `M-x lm-connect`.
`C-c C-e` sends the sexp before point, `C-c C-r` the region; results (and
errors) appear in the `*myos-lisp*` REPL buffer, which is also interactive.

### Verification

`python3 tools/lisp_serve_check.py` boots QEMU, types `lisp -serve` on the
serial console (paced, see above), then from the host: evals `(+ 1 2)` → `3`,
checks an error comes back over the socket, defines a function, disconnects,
reconnects, and calls it — proving the image survived the reconnect.

## 24.2 — system primitives (DEFUNs over syscalls)

`user/lm_sys.c` exposes the kernel to Lisp: every primitive is a DEFUN wrapping
one ulib syscall, registered into the image by `lm_sys_register()` right after
`lm_boot()`. This file is **user-build-only** — the core (`src/lm_core.c`) is
also compiled into the kernel for KTEST, and a kernel has no syscalls to wrap —
so the `lm.elf` Makefile rule links it and the kernel never sees it.

The vocabulary (fds/pids/statuses are fixnums, paths/data are strings, -1
means failure, exactly like C):

| Area      | Primitives |
|-----------|------------|
| processes | `(getpid)` `(fork)` `(exec path argv)` `(wait)`→`(pid . status)` `(exit [code])` `(kill pid sig)` `(sleep ms)` |
| files     | `(open path)` `(close fd)` `(fd-read fd n)` `(fd-write fd str)` |
| plumbing  | `(pipe)`→`(rfd . wfd)` `(dup2 old new)` |
| sockets   | `(socket 'stream\|'dgram)` `(bind fd port)` `(listen fd)` `(accept fd)` `(connect fd host port)` |
| machine   | `(shutdown)` |

Naming note: the byte-moving calls are `fd-read`/`fd-write` because the core
already owns `read` — in Lisp, `read` parses a *form* from the current input,
and shadowing it would break the REPL itself.

`(fork)` deserves a pause: it copies the whole process — the entire Lisp image
— through the kernel's copy-on-write machinery, and both sides resume mid-eval
of the same form. `(if (= (fork) 0) (exec ...) (wait))` is therefore the whole
Unix process model in one S-expression, and it is what `system.l` (24.3) builds
the shell from.

Verification: `python3 tools/lisp_sys_check.py` (boot-and-observe over the TCP
REPL — these primitives cannot run in the KTEST build by construction):
open/read/close on `/motd`, a pipe write→read roundtrip, fork/exit/wait,
fork/exec(`/bin/true`)/wait, exec-failure status, socket open/close.

## 24.3 — the shell, in Lisp (`user/lisp/system.l`)

Where `bootstrap.l` builds the *language* library out of the C primitives,
`system.l` builds the *operating-system* library out of the syscall primitives.
It is the Eshell model: the shell is not a separate program with its own little
language — it **is** the Lisp image, and commands are functions.

- `(run "hello" "arg" ...)` — fork → exec `/bin/hello` → wait → exit status.
  Variadic, which needed **rest parameters** in the core: a bare symbol in
  place of the parameter list binds all arguments (`(defun run cmdargs ...)`);
  `env_extend` now implements this (KTEST `lm: rest params + | symbol`).
- `(| left right)` — the pipeline, in its traditional spelling (`|` reads as an
  ordinary symbol). A macro: each stage is wrapped in a lambda and run in its
  own forked child, joined by `(pipe)` + `(dup2 … 0/1)`, mirroring `sh.c`'s
  `run_pipeline`. Because a stage only redirects **fd 1**, external programs
  and in-image Lisp compose freely: `(| (princ "abcde") (run "wc"))` → `5`.
  The parent closes both pipe ends before waiting (or the right stage would
  never see EOF) and returns the right stage's status, like `$?` in sh.
  (Over the network REPL this composes identically — see "the connection is
  the terminal" below.)
- Coreutils in Lisp: `(ls [path])` over a new `(readdir path)` primitive,
  `(cat path)` over `open`/`fd-read`/`princ`.
- `(repl)` — read→eval→print in one Lisp function, via the new `eval`
  primitive (KTEST `lm: eval primitive`).

Shipped via the initrd (`LISP_FILES += system` → `/lib/system.l`), loaded by
`/bin/lisp` right after `bootstrap.l`.

Verification: `python3 tools/lisp_shell_check.py` — serial phase: `(run
"hello")`, `(| (run "hello") (run "wc"))` → 22, `(| (princ "abcde") (run
"wc"))` → 5, `(cat "/motd")`, `(ls "/bin")`, `(run "sh")`/`exit` roundtrip;
TCP phase: `(cat)`, `(ls)`, `(run "hello")` → status over the socket, output
on the console.

## 24.4 — init IS the Lisp machine

The capstone, and deliberately the smallest diff of the phase: `src/initrd.c`
writes `lm_elf` (not `sh_elf`) to `/bin/init`. The machine now **boots into a
Lisp REPL as PID 1**. The C shell keeps its conventional home at `/bin/sh`,
demoted from "the userland" to "a command": `(run "sh")` drops into it,
`exit` falls back to Lisp — the Symbolics inversion on a Unix-shaped kernel.

The one behavioral requirement: **PID 1 must never exit**. `serial_repl()` in
`user/lm.c` checks `(sys_getpid() == 1)` and, as init, reopens the console
reader on end-of-input instead of returning — there is nothing left to reap
orphans or own the terminal if PID 1 dies. Run as an ordinary program (`lisp`
under `/bin/sh`), EOF still exits normally.

Verification: `python3 tools/lisp_init_check.py` — boot lands at `lisp> ` with
no `$ ` first; `(getpid)` → 1; `(run "hello")` and a pipeline work under init;
`(run "sh")` → the C shell works → `exit` → back to Lisp; the REPL still
answers afterwards. The 24.1b/24.2/24.3 checks all run against the flipped
boot too (the harness now boots to Lisp and starts the server with
`(run "lisp" "-serve")`).

## Post-24.4 — the connection is the terminal

First real-use feedback: `(run "http" "http://...")` over the Emacs REPL
printed its output on the guest's *serial console*, because an exec'd child
writes to its inherited fd 1 — the console — while only Lisp-level output went
through `lm_cur_out` (the socket). Expected under the original design, but not
the remote-shell experience the phase promises.

The fix is the most Unix move available: on `accept`, `serve_repl` stashes the
console on fds 13–15 and `dup2`s the socket onto **0/1/2** for the connection's
lifetime (restoring on disconnect) — the same redirection idiom the shell uses
for pipelines, applied to a session. Children now inherit the socket as stdio,
so external programs answer to the remote user.

The REPL's own Reader/Writer sit on fds 0/1 too (not on the conn fd directly),
and the *numbers* are the point: a pipeline stage `dup2`s its pipe onto fd 1,
and since `print`/`princ` write to "fd 1, whatever that currently is", in-image
stages land in the pipe exactly like external output. So
`(| (princ "abcde") (run "wc"))` → `5` now works over TCP identically to the
console. Lisp I/O and Unix plumbing compose because both speak fd numbers.

Trade-offs, accepted: programs started from a TCP session no longer echo on
the guest console, and a child that reads stdin reads the socket (telnet-ish —
also a feature). Covered by the updated `tools/lisp_shell_check.py` TCP phase.

## Phase 24: done

The OS boots into a live, redefinable Lisp image that is also its init, its
shell, and — over TCP, from Doom Emacs — its development environment. The
kernel did not change for any of it beyond one initrd line: every capability
the Lisp machine has, it gets through the same syscalls every C program uses.
