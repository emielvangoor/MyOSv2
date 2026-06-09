# MyOSv2 Lisp Machine — Design (Phase 24)

## Vision

Make a Lisp the **primary userland** of MyOSv2, in the spirit of Emacs and the
Symbolics/LMI Lisp machines — but on the pragmatic "Emacs-on-Unix" model: the C
kernel stays exactly as it is, and a Lisp image runs at EL0 as an ordinary
MMU-protected process, talking to the kernel through the existing syscalls.

The Lisp:
- fork/execs external ELF programs **and** runs in-image Lisp functions
  (the Eshell model) — protected external programs, a live Lisp world;
- eventually replaces `/bin/sh` as `init` (PID 1);
- accepts live connections from Doom Emacs over TCP, so you redefine the running
  OS from your editor — the true Lisp-machine experience.

Graphics / window manager are explicitly **out of scope** for this effort and
deferred to a later phase.

## Prior art

`~/Code/Sides/lm-lisp/` is a complete, working Emacs-architecture Lisp in ~1200
lines of host C: tagged 64-bit objects (low-3-bit tags), mark-and-sweep GC,
Lisp-2 (separate value/function slots), TCO in the evaluator, closures, macros,
and a `bootstrap.l` standard library. We port this core; we do not rewrite it.

## Architecture — one shared core, two hosts

```
┌──────────────────────────────────────────────────────────┐
│  bootstrap.l + system.l  (the "soul": shell, REPL, libs)  │  Lisp
├──────────────────────────────────────────────────────────┤
│  src/lm_core.c + src/lm.h   (reader · eval · printer · GC) │  C — SHARED
│   • no libc, no stdio: I/O via a Reader/Writer abstraction │
│   • allocation via lm_alloc()/lm_free() (platform-provided)│
├───────────────────────────────┬──────────────────────────┤
│  KERNEL build                 │  USER build               │
│  src/lm_platform.c            │  user/lm.c                │
│   lm_alloc→kmalloc            │   lm_alloc→malloc         │
│   + KTEST cases in tests.c    │   + REPL/serial/TCP/main  │
│   (red-green, gated by        │   sys primitives over     │
│    `make test` + pre-commit)  │   existing syscalls       │
└───────────────────────────────┴──────────────────────────┘
                  │                          │
                  ▼                          ▼
       MyOSv2 kernel (UNCHANGED): MMU, sched, fork/COW, ELF,
       VFS, signals, TCP/IP + BSD sockets, poll
```

The key move: **`src/lm_core.c` is the single source of truth**, compiled into
the kernel (so KTEST can red-green the reader/eval/printer/GC on-target) *and*
into the `lm` user ELF (the real REPL). Separate binaries → no symbol clash; the
platform layer (`lm_alloc`/`lm_free`/`lm_write`) is the only thing that differs.

## Porting the host core to freestanding C

The `lm` core assumes a hosted libc. We sever every dependency:

| Host dependency        | Replacement                                            |
|------------------------|--------------------------------------------------------|
| `FILE*`, `fgetc/ungetc`| `Reader` struct: STRING or FD source + small pushback   |
| `printf/fprintf/fputc` | `Writer` struct: capture-to-buffer or `lm_write(fd,…)`  |
| `calloc/free`          | `lm_alloc(size)` (zeroing) / `lm_free` (platform)       |
| `strdup/strlen/strcmp` | small static helpers in `lm_core.c` (freestanding)      |
| `strtol`               | `parse_fixnum()` for the reader                          |
| `isspace`              | static helper                                           |
| `snprintf` (in strcat) | small `itoa`                                            |
| `setjmp/longjmp`       | `user/lm_setjmp.S` (save x19–x30, sp) — error recovery   |
| `signal`/Ctrl-C        | kernel `signal(SIGINT,…)` → `longjmp` back to the prompt |

`malloc` in MyOSv2 userland is 16-byte aligned (≥ the 8-byte alignment the tag
scheme needs), so the tagged-pointer representation ports unchanged.

## GC — conservative C-stack scanning (the chosen approach)

The host `lm` has a known bug: GC never scans C-stack roots, so it dodges the
problem with a huge threshold and only collecting between REPL iterations. `init`
runs forever, so GC must actually work mid-computation. We fix it with
**conservative stack scanning**:

1. At program start, record `lm_stack_base` (high address) via
   `__builtin_frame_address(0)` in `umain`.
2. On `gc_collect`: spill callee-saved registers to the stack (via `setjmp` into
   a local buffer), read the current SP, and scan every aligned word in
   `[sp, lm_stack_base)` **plus** the saved-register buffer.
3. For each word `w`: mask the low 3 tag bits and check membership in the heap
   object list (`is_heap_object`, fast-rejected by tracked lo/hi address bounds).
   If it is a real object, `gc_mark(w)` — `w` keeps its true tag, so marking
   recurses correctly. False positives merely retain garbage (acceptable).
4. With roots now safe, `gc_alloc` triggers `gc_collect` when `alloc_count`
   exceeds the threshold — collection is safe mid-eval because live `Lobj`s held
   in C locals are found on the stack.

In the kernel KTEST build, GC is tested via explicitly-held roots (mark/sweep
correctness), not stack scanning; stack scanning is exercised by the real ELF.

## Phasing

- **24.1 — Port the core + serial REPL.** `src/lm.h`, `src/lm_core.c`,
  `src/lm_platform.c`, `user/lm.c` (`/bin/lisp`), `user/lm_setjmp.S`. `bootstrap.l`
  shipped via the initrd to `/lib/bootstrap.l` and `load`ed at start. Conservative
  GC working. KTEST cases for reader/eval/printer/GC (red→green). Interactive REPL
  over serial with line editing + Ctrl-C recovery.
- **24.1b — Emacs ↔ TCP (right after 24.1, per request).** `lm` gains a network
  REPL: `listen`/`accept` on a port (blocking sleep/wake — no polling), read-eval-
  print over the socket. QEMU forwards a host port. Ship `lm-mode.el` +
  instructions so `C-c C-e` in Doom Emacs evals into the live guest image.
- **24.2 — System primitives.** `DEFUN`s wrapping existing syscalls: `(fork)`
  `(exec path argv)` `(wait)` `(open)` `(close)` `(read fd n)` `(write fd s)`
  `(pipe)` `(dup2)` `(kill)` `(getpid)` `(socket/bind/connect/listen/accept)`,
  plus `(exit)`. Each teaching-level documented.
- **24.3 — The shell in Lisp (`system.l`).** Eshell-hybrid, pure S-expressions:
  `(run "hello" "arg")` → fork→exec `/bin/hello`→wait; `(| (run "a") (run "b"))`
  → pipe+dup2 between forked children; in-image `(defun)`s callable the same way;
  a `repl` built from `read`/`eval`/`print`.
- **24.4 — Flip `init` to Lisp.** `initrd.c`: `/bin/init` → `lm_elf` (keep
  `/bin/sh` as a C fallback). One-line change, done last so the system is always
  bootable during development.

## Testing (TDD, gated by `make test` + pre-commit)

- Portable core: KTEST cases in `src/tests.c` — eval forms from strings, print to
  a capture buffer, assert on output (`(+ 1 2)`→`"3"`, list ops, closures, macros,
  `gc` keeps held roots). Written test-first, red before green.
- Integration (primitives, shell, TCP, init flip): verified by booting under QEMU
  and observing behavior, since they are inherently userland/over-the-wire.
- README capability summary refreshed as part of the phase (project convention).

## Out of scope (later phases)

Graphics / virtio-gpu / framebuffer / window manager; `save-image` (unexec) to
`/disk`; floats and hash tables (not needed for the shell).
