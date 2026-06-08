# ARM64 Learning OS — Phase 5 Design (Threads, Context Switching & Scheduling)

**Date:** 2026-06-08
**Status:** Approved

## Goal

Give MyOSv2 multitasking: a **thread** abstraction (own stack + saved register
context), a **context switch** in assembly, a round-robin **scheduler** with a
cooperative `yield()`, and **timer-driven preemption**. End result: multiple
kernel threads visibly interleave on their own.

First **test-first** phase: the cooperative-path tests are written before the
scheduler exists (RED), then implemented to GREEN. All threads run at EL1 in one
shared address space (user mode is Phase 6). Builds on Phase 0–4 (timer,
exceptions, heap) and the test harness.

## Saved context & the switch (`switch.S`)

A cooperative switch only needs the AArch64 **callee-saved** registers (the
caller already expects `x0–x18` to be clobbered across a call):

```c
struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28; // offsets 0..72
    uint64_t fp;   // x29   offset 80
    uint64_t lr;   // x30   offset 88
    uint64_t sp;   //       offset 96
};  // 13 * 8 = 104 bytes
```

`cpu_switch(struct context *old, struct context *new)` (assembly): store the 13
registers into `old`, load them from `new`, `ret`. The `ret` returns into
`new->lr` — i.e. resumes the other thread. Register-to-struct offsets MUST match
the struct above.

`thread_trampoline` (assembly): the initial `lr` of a new thread. It unmasks IRQs
(`msr daifclr, #2`), moves `x20`→`x0` (arg), `blr x19` (call fn), then
`bl thread_exit` if fn returns.

## Threads & scheduler (`sched.h` / `sched.c`)

```c
enum thread_state { THREAD_RUNNABLE, THREAD_RUNNING, THREAD_EXITED };

struct thread {
    struct context ctx;       // saved registers (FIRST member; switch.S uses &ctx)
    uint8_t *stack;           // kmalloc'd stack base (NULL for the boot thread)
    enum thread_state state;
    int id;
    struct thread *next;      // circular run-queue link
};
```

- `sched_init(void)` — register the currently-running code (boot/kmain) as
  thread 0 so we can switch away from and back to it; reset the run-queue.
- `thread_create(void (*fn)(void *), void *arg)` — `kmalloc` a `struct thread`
  and a 16 KiB stack; set `ctx.sp` = top of stack, `ctx.lr` = `thread_trampoline`,
  `ctx.x19` = fn, `ctx.x20` = arg; mark RUNNABLE; link into the run-queue. Returns
  the thread.
- `schedule(void)` — pick the next RUNNABLE thread round-robin from `current`,
  then `cpu_switch(&current->ctx, &next->ctx)`; update `current`.
- `yield(void)` — cooperative: just calls `schedule()`.
- `thread_exit(void)` — mark `current` EXITED, unlink it, `schedule()` away
  (never returns).
- `sched_started(void)` — returns whether `sched_init` has run (used to gate
  preemption in the IRQ handler).

Stacks: 16 KiB via `kmalloc`, used top-down, 16-byte aligned.

## Preemption (timer)

In `irq_handler`, after acknowledging/EOI-ing the timer interrupt, call
`schedule()` when `sched_started()` — so a thread that never yields is still
switched out. Notes:
- Exceptions auto-mask IRQs on entry; `thread_trampoline` re-enables them so a
  freshly started thread can itself be preempted.
- Each thread runs on its own stack, so the in-progress trap frame stays with the
  preempted thread and is restored when it next runs.

## Testing (test-first; cooperative path is deterministic)

Written BEFORE the implementation (watch them fail, then build):

- `test_thread_create_context`: after `thread_create(fn, arg)`, the returned
  thread has `state == THREAD_RUNNABLE`, `stack != NULL`, `ctx.sp` within
  `[stack, stack+size]`, `ctx.lr == (uint64_t)thread_trampoline`,
  `ctx.x19 == (uint64_t)fn`, `ctx.x20 == (uint64_t)arg`.
  (`thread_trampoline` is declared `extern` so the test can compare.)
- `test_round_robin_order`: a global `int order[…]`, `n=0`. Two worker threads
  each do `for k in 0..2 { order[n++] = id; yield(); }` then return (→ exit).
  The main/boot thread calls `sched_init`, creates both, then `while (n < 6)
  yield();`, and asserts `order == {1,2,1,2,1,2}`. This proves context switching,
  stack/register preservation, and round-robin order deterministically.

Preemption is verified by observation in the demo (interleaved output), not unit
tested (timing-dependent) — explicitly noted.

Tests run during `run_self_tests()` (before the real scheduler/timer setup in
`kmain`), so they execute with interrupts off → fully deterministic. They run all
workers to exit, leaving only the boot thread; `kmain` re-inits the scheduler
afterward.

## Demo (`kmain`)

After the existing startup, `kmain` calls `sched_init()`, creates 3 threads that
each loop `kprintf("X")` (X = `A`/`B`/`C`) with a short busy-delay and never
yield, enables timer preemption, and lets them run. Output shows interleaved runs
(e.g. `AAAABBBBCCCC…`), proving the timer time-slices. Runs forever (`Ctrl-C`).

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/switch.S` | `cpu_switch` + `thread_trampoline` (assembly) |
| `src/sched.h` / `sched.c` | thread struct, run-queue, create/yield/schedule/exit/init |
| `src/tests.c` | (modified) add thread-create + round-robin tests (written first) |
| `src/exceptions.c` | (modified) timer IRQ calls `schedule()` when `sched_started()` |
| `src/kmain.c` | (modified) set up scheduler + the 3-thread preemption demo |

All new code heavily commented (project preference).

## Success criteria

- New `[PASS]` lines for `thread create context` and `round-robin order` (after a
  RED→GREEN cycle).
- `make run` demo shows interleaved output from the 3 non-yielding threads →
  preemption works.
- All prior tests still pass; `make test` exits 0; the commit gate stays intact.

## Out of scope (later phases)

User mode / EL0, per-thread address spaces, thread priorities, sleep/wait queues,
joining, SMP/multi-core. Deferred to Phase 6+.
