# Phase 18 notes — signals

## What changed

Asynchronous signals: `kill(pid, sig)` posts a signal; a process either runs a
**handler** it installed or takes the **default action** (terminate). And a real
**Ctrl-C**:

```
$ loop
looping (Ctrl-C to stop)...     ^C
[exit 130]                       # 128 + SIGINT(2): terminated by the default action

$ catch
catch: press Ctrl-C three times
caught SIGINT                    ^C   (handler ran, then execution resumed)
caught SIGINT                    ^C
caught SIGINT                    ^C
catch: bye
[exit 0]
```

## State + posting

Each thread has `sig_pending` (a bitmask), `sig_handler[32]` (user handler VAs,
NULL = default), and `sig_tramp` (the user trampoline). `signal_send` sets the
pending bit and wakes the thread if it's sleeping. `sched_kill(pid, sig)` finds
the thread by id. `SIGKILL` is uncatchable (`signal_action` always returns the
default for it).

## Delivery (the interesting part)

Signals are delivered **on the way back to EL0** — at the end of the syscall
handler and on a timer IRQ that interrupted user mode (`SPSR.M == EL0t`).
`signals_deliver(tf)`:
- **default** (no handler): `thread_exit(128 + sig)`.
- **handler**: push a copy of the interrupted trap frame onto the user stack,
  then point the trap frame at the handler — `x0 = sig`, `elr = handler`,
  `lr = sig_tramp`. The handler runs at EL0; when it `ret`s it lands in the ulib
  `__sigreturn` stub, which calls `SYS_SIGRETURN`. The kernel copies the saved
  frame back over the trap frame, so the original code resumes exactly where the
  signal interrupted it.

This reuses the Phase-17 EL1 COW-fault fix: writing the saved frame onto a (still
COW) user stack page faults at EL1 and is copied transparently.

## Ctrl-C without job control

No process groups yet, so: `sched_wait` marks the child it blocks on as the
**foreground** thread. Each timer tick, the IRQ handler polls the UART while a
foreground program runs (it isn't reading stdin, so consuming the byte is fine);
a `0x03` posts `SIGINT` to it. When the child dies, `sched_wait` clears
foreground and the shell reads the console again.

## Testing

3 kernel tests, test-first: `kill` sets the pending bit; `sched_kill` by pid
(and -1 for an unknown pid); the action policy (default vs handler, SIGKILL
uncatchable). The handler trampoline + sigreturn and Ctrl-C are verified live by
`/bin/loop` (exit 130) and `/bin/catch` (handler runs, then `bye`).

## Limits

A tiny signal set; no `sigprocmask`/masks, no queued realtime signals, no
`SA_RESTART`/restartable syscalls, no job control / process groups.
