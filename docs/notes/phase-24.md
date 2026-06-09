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
limitation, not a Lisp one — human-speed typing is fine, and the upcoming
Emacs-over-TCP path (24.1b) bypasses the tty entirely.
