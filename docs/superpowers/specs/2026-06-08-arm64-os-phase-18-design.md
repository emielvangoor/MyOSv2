# MyOSv2 — Phase 18 Design (signals)

**Date:** 2026-06-08
**Status:** Approved (autonomous build, roadmap pre-approved)

## Goal

Asynchronous notification between/within processes, and a real **Ctrl-C**.
`kill(pid, sig)` posts a signal; a process can install a **handler** or take the
**default action** (terminate). Pressing Ctrl-C delivers `SIGINT` to the
foreground program.

Builds on the process lifecycle (13) and the trap/return path (2).

## Model

A small fixed set of signals; what matters is `SIGINT (2)`, `SIGKILL (9)`,
`SIGTERM (15)`. Each thread gains:
```c
uint64_t sig_pending;                 // bitmask of posted-but-undelivered signals
uint64_t (*sig_handler[32])(int);     // per-signal user handler (0 = default)
```
`kill` ORs the bit into the target's `sig_pending` and wakes it if sleeping.
Signals are **delivered when returning to EL0** — checked in `el0_sync_handler`
after a syscall, and on a timer IRQ taken from EL0.

For each pending signal (lowest first):
- **default** (no handler): terminate the process (`thread_exit(128 + sig)`).
  `SIGKILL` always defaults (uncatchable).
- **handler installed**: run it in user mode (below).

## Delivering to a user handler

We make the handler run at EL0, then resume the interrupted code. On delivery
(`signals_deliver(tf)`):
1. Push a copy of the interrupted **trap frame** onto the user stack (16-aligned)
   and lower `tf->sp_el0` past it.
2. `tf->x[0] = sig` (the handler's argument).
3. `tf->elr = handler`; `tf->x[30] (lr) = sig_trampoline` — a tiny user stub the
   process registered (so when the handler `ret`s, it lands there).
4. The trampoline calls `SYS_SIGRETURN`, which **restores** the saved trap frame
   from the user stack, so the original code resumes exactly where it was.

`SYS_SIGNAL(sig, handler, trampoline)` records the handler (and, once, the
trampoline address from `ulib`).

## Ctrl-C → SIGINT

No job control yet, so we keep it minimal: the kernel tracks a **foreground
thread** (the child the shell is currently `wait`ing on). On each timer tick the
handler polls the UART; a `0x03` (Ctrl-C) byte posts `SIGINT` to the foreground
thread. With the default action that terminates the looping program and the shell
regains control.

## Syscalls

```
SYS_KILL      = 20   // x0 = pid, x1 = sig  -> 0 / -1
SYS_SIGNAL    = 21   // x0 = sig, x1 = handler, x2 = trampoline -> 0 / -1
SYS_SIGRETURN = 22   // restore the saved frame (used by the trampoline)
```

## User side (`ulib`) + demo

`ulib`: `signal(sig, handler)` (passes the ulib `__sigreturn` trampoline),
`kill(pid, sig)`. A `__sigreturn` stub: `svc SYS_SIGRETURN`.

`/bin/catch`: installs a `SIGINT` handler that prints "caught!" and a counter,
loops forever; after N catches it `exit`s. `/bin/loop`: loops forever with no
handler (Ctrl-C terminates it by default). The shell marks the child it waits on
as foreground, so Ctrl-C reaches it.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/signal.h`/`signal.c` | pending/handler state, `signals_deliver`, `signal_send` |
| `src/sched.h`/`sched.c` | per-thread signal fields; foreground thread; `kill` by pid |
| `src/exceptions.c` | call `signals_deliver` on return to EL0 |
| `src/timer.c`/`irq` | poll UART for Ctrl-C each tick → SIGINT to foreground |
| `src/syscall.*` | `SYS_KILL`/`SYS_SIGNAL`/`SYS_SIGRETURN` |
| `user/syscalls.h`, `user/ulib.*` | `signal`, `kill`, `__sigreturn` |
| `user/catch.c`, `user/loop.c` | demo programs |
| `src/tests.c` | signal tests (test-first) |
| `docs/notes/phase-18.md` | notes |

## Testing (test-first, kernel-level)

The pending/default/kill machinery is unit-tested; handler delivery + Ctrl-C are
verified live.

1. `test_kill_sets_pending` — `signal_send(t, SIGINT)` sets the bit in
   `t->sig_pending`.
2. `test_kill_by_pid` — `sched_kill(pid, SIGTERM)` finds the thread and posts it;
   an unknown pid returns -1.
3. `test_default_terminates` — a thread with a pending, handler-less signal:
   `signals_pending_default(t)` reports "would terminate" (the policy function is
   pure/testable; the actual `thread_exit` is observed live).
4. `test_handler_recorded` — `signal_set_handler(t, SIGINT, H, TR)` stores `H`;
   `SIGKILL` can't be caught (stays default).
5. `test_sigreturn_restores` — build a trap frame, deliver a handler (frame saved
   to a buffer acting as the user stack), then the restore yields the original
   frame bytewise.

## Success criteria

- 5 tests pass (test-first); prior tests stay green; gate holds.
- Live: `loop` then Ctrl-C terminates it and returns to the shell; `catch`'s
  handler prints on Ctrl-C and exits after N; `kill` from one program ends
  another.

## Out of scope

Full POSIX signal set / semantics; signal masks (`sigprocmask`); queued realtime
signals; `SA_RESTART`; restartable syscalls; job control / process groups.
