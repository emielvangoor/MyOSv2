# MyOSv2 — Scheduler Refinement Design (priorities, sleep, robustness)

**Date:** 2026-06-08
**Status:** Approved

## Goal

Harden the Phase 5 scheduler into a real one: **strict-priority** scheduling
(round-robin within a level), a **`sleep`** primitive (blocking + timer wakeup),
a dedicated **idle thread**, and **IRQ-masked critical sections** so the run-queue
can't be corrupted by the timer interrupt. Developed test-first.

Builds on Phase 5 (threads, context switch, round-robin, 1 kHz tick / 10 ms slice).

## Priority model

Strict priority: always run the highest-priority RUNNABLE thread; round-robin
among threads of equal priority. **Higher numeric `priority` = more important.**
A high-priority thread that never sleeps deliberately starves lower ones (that's
what `sleep` is for). No aging/weighting.

## Thread & state changes

```c
enum thread_state { THREAD_RUNNABLE, THREAD_RUNNING, THREAD_SLEEPING, THREAD_EXITED };

struct thread {
    struct context ctx;
    uint8_t *stack;
    enum thread_state state;
    int id;
    int priority;          // higher = more important
    uint64_t wake_tick;    // jiffy to wake at (when SLEEPING)
    struct thread *next;   // circular ring of ALL threads
};

struct thread *thread_create(void (*fn)(void *), void *arg, int priority);
```

A global `jiffies` counter (incremented each tick in `sched_tick`) is the sleep
clock.

## Scheduler (`schedule`)

The ring holds all threads. `schedule()`:
1. If `current` is `RUNNING`, set it `RUNNABLE` (it's yielding the CPU). If the
   caller already set `SLEEPING`/`EXITED`, leave that.
2. Scan the whole ring from `current->next`, picking the highest-priority
   `RUNNABLE` thread; "strictly greater" comparison makes ties resolve to the
   first one after `current` → round-robin within a level.
3. The **idle thread is always RUNNABLE at the lowest priority**, so the scan is
   never empty.
4. Mark the winner `RUNNING`; if it differs from `current`, reset the time slice
   and `cpu_switch`.

**Exit:** `thread_exit` marks the thread `EXITED` (a tombstone the scan skips)
rather than unlinking it — simpler and avoids pointer races. The struct/stack
leak (already accepted).

**Idle thread:** `sched_init` makes the boot context the idle thread: priority
`-1` (below any created thread), always RUNNABLE, runs the `kmain` `wfi` loop.

## Sleep & wakeup

- `void sleep_ticks(uint64_t ticks)` — IRQ-masked: set `current->wake_tick =
  jiffies + ticks`, `state = SLEEPING`, `schedule()` (switch away); restore IRQs
  when later resumed.
- `void sleep_ms(uint64_t ms)` — `sleep_ticks(ms)` (1 ms = 1 tick at `TIMER_HZ`
  = 1000).
- In `sched_tick()` (every tick, IRQ context): `jiffies++`, then scan the ring
  and wake (`SLEEPING → RUNNABLE`) any thread with `wake_tick <= jiffies`. If a
  woken thread outranks `current`, signal an immediate reschedule (return 1) so
  priority preemption is prompt.

## Concurrency safety ("rock solid")

Invariant: **scheduler data structures are only touched with IRQs masked.**
- The preemption path (`irq_handler`/`sched_tick`) is already IRQ-masked on
  exception entry.
- Thread-context entry points (`yield`, `sleep_ticks`, `thread_create`,
  `thread_exit`) wrap their critical sections in `irq_save()` / `irq_restore()`
  (read+set / restore `DAIF.I`), declared `static inline` in `sched.h`.
- So every `cpu_switch` runs with IRQs masked; threads re-enable on resume (new
  threads via the trampoline's `daifclr`; preempted threads via `eret` restoring
  `SPSR`; cooperative paths via `irq_restore`). Single core + masked IRQs = mutual
  exclusion, no locks.

## Testing (test-first, deterministic)

- **`test_priority_order`** (Task: priorities): create a low-priority worker,
  then a high-priority worker (each logs its level and exits); the idle thread
  yields until both ran; assert the **high-priority ran first** (`[2,1]`) despite
  being created second. RED with plain round-robin (would give `[1,2]`); GREEN
  after priority-aware `schedule`.
- **`test_sleep_wakes_after_ticks`** (Task: sleep): a worker logs `S`,
  `sleep_ticks(3)`, logs `W`; the test thread drives `sched_tick()`+`yield()` and
  asserts the worker stays asleep for 2 ticks and wakes on the 3rd (`S` then `W`,
  not before). Deterministic because the test drives the clock. RED = undefined
  `sleep_ticks`; GREEN after implementing sleep/wake.

Existing `test_round_robin_order` and `test_time_slice_expiry` keep passing
(workers get equal priority → round-robin within the level). `thread_create`
call sites are updated to pass a priority.

## Demo (`kmain`)

A high-priority thread `A` prints a short burst then `sleep_ms(...)`; two equal
lower-priority threads `B`/`C` print continuously. Output: while `A` sleeps,
`B`/`C` round-robin (`BCBC…`); when `A` wakes it preempts them (`AAAA`), then
sleeps again — demonstrating priority, sleep, and round-robin-within-level
together.

## Files & changes

| File | Change |
|------|--------|
| `src/sched.h` | states (+SLEEPING), `priority`/`wake_tick`, `thread_create(…, priority)`, `sleep_ticks`/`sleep_ms`, `irq_save`/`irq_restore` |
| `src/sched.c` | priority scan, idle thread, `jiffies`, sleep/wake, IRQ-masked sections, tombstone exit |
| `src/tests.c` | update `thread_create` calls; add `test_priority_order`, `test_sleep_wakes_after_ticks` (written first) |
| `src/kmain.c` | priority+sleep demo; pass priorities |
| `docs/notes/phase-5.md` | document priorities, sleep, concurrency model |

## Success criteria

- New `[PASS] sched: priority order` and `[PASS] sched: sleep wakes after ticks`
  (each after a RED→GREEN cycle); all prior tests still green; `make test` exit 0;
  gate intact.
- Demo shows priority preemption + `B`/`C` round-robin while `A` sleeps.
- Long runs are stable (idle thread covers all-asleep; IRQ-masked sections prevent
  run-queue corruption).

## Out of scope

Aging/anti-starvation, weighted shares, wait queues / condition variables,
multi-core. User mode is Phase 6 (next).
