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

## Linux-style tick vs. time slice (follow-up enhancement)

Originally the timer fired at 1 Hz and we preempted on *every* tick, so the
quantum was a full second — threads ran in big blocks (`AAAA…BBBB…`). Linux
**decouples** two things:

- the **tick**: a fast, fixed heartbeat (`TIMER_HZ`, now **1000 Hz** = 1 ms) used
  for timekeeping and as the scheduling clock;
- the **time slice / quantum**: how long a thread runs before preemption
  (`SCHED_TIME_SLICE`, now **10 ticks** = 10 ms).

The timer still interrupts every 1 ms, but `sched_tick()` decrements a
`slice_left` counter and only returns "reschedule" when it hits zero (every 10th
tick). `irq_handler` switches only on that signal:

```c
if (id == 30) { timer_handle_irq(); resched = sched_tick(); }
gic_eoi(id);
if (resched) schedule();
```

`slice_left` resets to a full slice in `sched_init` and whenever `schedule`
actually switches, so each newly-running thread gets a fresh quantum.
`sched_tick()` is pure, deterministic logic, so it's unit-tested
(`test_time_slice_expiry`) — RED (undefined symbol) → GREEN.

Result: with a 10 ms slice and ~8 ms-per-character threads, the demo now shows
fine interleaving (`AABBCCABCABC…`) instead of per-second blocks.

Real Linux goes further (CFS: variable, priority-weighted slices, virtual
runtime); we keep fixed round-robin with just the tick/slice split.

### Gotcha: QEMU timer is wall-clock paced

QEMU (TCG, no `-icount`) advances the generic-timer counter with *host* real
time, not guest instructions. So if QEMU is starved of host CPU (e.g. another
process pegs the core), the guest spends its few cycles servicing the real-time
1 kHz tick and the threads barely advance — output appears to "stall." It's a
measurement artifact, not a kernel bug; an unstarved run prints ~85 letters/sec
steadily.

## Scheduler refinement: priorities, sleep, robustness

The Phase 5 scheduler was hardened into a real one.

**Priorities (strict + round-robin within a level).** Each thread has an `int
priority` (higher = more important). `schedule()` scans the whole ring and picks
the highest-priority `RUNNABLE` thread; using a *strictly-greater* comparison
while scanning from `current->next` means equal-priority threads alternate
(round-robin within the level). A high-priority thread that never sleeps starves
lower ones — by design; that's what `sleep` is for.

**Idle thread.** `sched_init` makes the boot context the idle thread: priority
`-1` (below anything created), always runnable, running `kmain`'s `wfi` loop. So
there is always a runnable thread and `schedule()` never comes up empty (e.g.
when every worker is asleep).

**Sleep.** `sleep_ticks(n)` / `sleep_ms(ms)` set `current->wake_tick = jiffies +
n`, mark the thread `SLEEPING`, and `schedule()` away. A `jiffies` counter
advances each tick inside `sched_tick()`, which also scans the ring and wakes
(`SLEEPING → RUNNABLE`) any thread whose deadline passed. If a woken thread
outranks the running one, `sched_tick` returns "reschedule" so priority preemption
is prompt.

**Tombstone exit.** `thread_exit` just marks the thread `EXITED`; the scheduler
skips tombstones. Simpler than unlinking (no pointer surgery, fewer race
windows). The thread struct + stack leak — accepted for now.

**Concurrency (the "rock solid" part).** The run-queue and sleep state are touched
by both thread code and the timer IRQ. The invariant: *scheduler structures are
only modified with IRQs masked.* The IRQ path is already masked on exception
entry; thread-context entry points (`yield`, `sleep_ticks`, `thread_create`,
`thread_exit`) wrap their critical sections in `irq_save()`/`irq_restore()`
(set/restore `DAIF.I`). On a single core, masked IRQs give mutual exclusion with
no locks. Every `cpu_switch` runs with IRQs masked; threads re-enable on resume
(new threads via the trampoline's `daifclr`, preempted threads via `eret`
restoring `SPSR`, cooperative paths via `irq_restore`).

**Demo.** A high-priority thread `A` prints 5 chars then `sleep_ms(40)`; two
equal lower-priority threads `B`/`C` print continuously. Output:
`BBBBCCCC…AAAAA…BBBBCCCC…` — `B`/`C` round-robin while `A` sleeps, then `A`
preempts on wake.

**Tested (test-first):** `test_priority_order` (high runs before low despite
creation order) and `test_sleep_wakes_after_ticks` (worker sleeps exactly 3 ticks,
the test drives the clock deterministically). Preemption/timing remain
observed-only.
