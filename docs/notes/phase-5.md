# Phase 5 notes — Threads, Context Switching & Scheduling

## What changed

MyOSv2 went from a single flow of execution to **multitasking**. It now has
kernel threads, a real assembly context switch, a round-robin scheduler with a
cooperative `yield()`, and **timer-driven preemption**. The demo shows three
threads (A/B/C) that never yield, yet the timer rotates them — output appears in
~1-second chunks (`AAAA…BBBB…CCCC…AAAA…`), one per scheduling quantum.

This was the first **test-first** phase: the cooperative tests were written
before the scheduler existed (RED = linker `undefined reference`), then
implemented to GREEN.

## What a thread is

A thread = an independent execution flow with its **own stack** and a saved
**register context**. Switching threads means: save the current registers, load
another thread's, and resume. Each of our threads gets a 16 KiB stack from
`kmalloc` (Phase 4 paying off).

## The context switch (`cpu_switch`)

A *cooperative* switch only saves the **callee-saved** registers (`x19–x28`, `fp`,
`lr`, `sp`). It doesn't need `x0–x18` because the ARM calling convention already
lets a function call clobber those — and `cpu_switch` is reached as a normal call.

`cpu_switch(old, new)` stores 13 registers into `old`, loads them from `new`, then
`ret`. The trick: `ret` jumps to `new->lr`, which is *the other thread's* saved
return address — so a single `ret` resumes a different thread. The `struct
context` field offsets in `sched.h` must exactly match the `stp/ldp` offsets in
`switch.S`.

## Birth of a thread (`thread_create` + `thread_trampoline`)

`thread_create` hand-crafts an initial context so the *first* switch lands
correctly: `sp` = top of the new stack, `lr` = `thread_trampoline`, and `fn`/`arg`
stashed in `x19`/`x20` (which `cpu_switch` restores). The first `cpu_switch` into
the thread `ret`s into `thread_trampoline`, which:
1. **re-enables IRQs** (`msr daifclr, #2`) — critical, because if the switch
   happened from inside an IRQ handler, IRQs were masked, and the new thread must
   be preemptible;
2. calls `fn(arg)`;
3. calls `thread_exit` if `fn` returns.

## The scheduler

A circular linked list (ring) of threads + a `current` pointer.
- `sched_init` registers the currently-running code as the boot thread (a ring of
  one). Its context is captured on the first switch away.
- `schedule` picks the next non-exited thread after `current` and `cpu_switch`es.
- `yield` = call `schedule` voluntarily (cooperative).
- `thread_exit` unlinks the current thread and switches away forever.

New threads are inserted at the ring **tail** (just before `current`) so they run
in creation order — that's why the round-robin test sees `1,2,1,2,1,2`.

## Preemption

The timer IRQ handler, after EOI, calls `schedule()` when `sched_started()`. So a
thread that never calls `yield` is still switched out involuntarily. Key points:
- We **EOI before switching** so the GIC is ready to deliver the next interrupt.
- The switch happens on the **current thread's own stack**, so its in-progress
  trap frame stays with it and is restored when it next runs.
- Exception entry auto-masks IRQs; the trampoline re-enables them for new threads;
  a resumed thread's `eret` restores `SPSR` (IRQs enabled), so masking stays
  consistent.

## Testing

Cooperative `yield` is **deterministic**, so we unit-test it: two workers append
their id and yield; the order is exactly `1,2,1,2,1,2`. That one test exercises
context switching, stack/register preservation, and round-robin ordering. The
tests run during `run_self_tests()` — *before* interrupts are enabled — so no
preemption perturbs them.

**Preemption is not unit-tested** (timing-dependent); it's verified by observing
the interleaved A/B/C output.

## Known simplifications (revisit later)

- An exited thread's stack **leaks** (no `kfree` of stacks yet).
- After the round-robin test, one worker may remain suspended in the ring until
  the next `sched_init` resets it (harmless; the boot context is unaffected).
- No priorities, sleeping/blocking, wait queues, `join`, or SMP. No user mode yet
  (all threads share EL1 and one address space) — that's Phase 6.
