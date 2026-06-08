# MyOSv2 — Scheduler Refinement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add strict-priority scheduling, `sleep`, an idle thread, and IRQ-masked critical sections to the Phase 5 scheduler.

**Architecture:** A circular ring of all threads; `schedule()` picks the highest-priority RUNNABLE (round-robin within a level); an always-runnable idle thread (priority -1) is the boot context; `sleep_ticks`/`sched_tick` use a `jiffies` clock; thread-context entry points mask IRQs so the timer can't corrupt the ring.

**Tech Stack:** Freestanding C + AArch64, the Phase 5 scheduler/timer.

**Note (test-first):** Task 1 is a behavior-preserving refactor guarded by the existing tests. Tasks 2–3 add new behavior test-first (genuine RED). `make test` must stay green; the gate is active. All code heavily commented.

---

## File structure

| File | Change |
|------|--------|
| `src/sched.h` | states (+SLEEPING), `priority`/`wake_tick`, `thread_create(…, priority)`, `sleep_ticks`/`sleep_ms`, `irq_save`/`irq_restore` |
| `src/sched.c` | priority scan, idle thread, `jiffies`, sleep/wake, IRQ-masked sections, tombstone exit |
| `src/tests.c` | update `thread_create` calls; add priority + sleep tests (test-first) |
| `src/kmain.c` | priority+sleep demo |
| `docs/notes/phase-5.md` | document the refinements |

---

## Task 1: Struct/API + IRQ-safety (behavior-preserving refactor)

**Files:**
- Modify: `src/sched.h`
- Modify: `src/sched.c`
- Modify: `src/tests.c`
- Modify: `src/kmain.c`

The existing tests (round-robin, time-slice) are the safety net: behavior must
not change. We add the priority field + sleep declarations + IRQ-masked sections,
and update call sites; `schedule()` stays round-robin for now.

- [ ] **Step 1: Replace `src/sched.h` with the full new interface**

```c
// sched.h -- kernel threads and the priority round-robin scheduler.
// =================================================================
//
// A thread is an independent flow of execution with its own stack and saved
// register state. The scheduler runs the highest-priority RUNNABLE thread,
// round-robin among equal priorities, with a 1 ms tick and a 10 ms time slice.
#pragma once
#include <stdint.h>

// Callee-saved CPU state for a cooperative switch (offsets MUST match switch.S).
struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28; // @0 .. @72
    uint64_t fp;   // x29   @80
    uint64_t lr;   // x30   @88
    uint64_t sp;   //       @96
};

enum thread_state {
    THREAD_RUNNABLE,   // ready to run
    THREAD_RUNNING,    // currently on the CPU
    THREAD_SLEEPING,   // blocked until wake_tick
    THREAD_EXITED      // finished (a tombstone the scheduler skips)
};

struct thread {
    struct context ctx;    // MUST be first: switch.S uses &thread (== &thread->ctx)
    uint8_t *stack;        // kmalloc'd stack base (NULL for the idle thread)
    enum thread_state state;
    int id;
    int priority;          // higher number = more important
    uint64_t wake_tick;    // jiffy to wake at (when SLEEPING)
    struct thread *next;   // circular run-queue link
};

// Assembly (switch.S):
void cpu_switch(struct context *old, struct context *newc);
void thread_trampoline(void);

// Scheduler:
void sched_init(void);                                            // register idle thread
struct thread *thread_create(void (*fn)(void *), void *arg, int priority);
void yield(void);                                                 // cooperative switch
void schedule(void);                                             // pick highest-prio + switch
void thread_exit(void);                                          // end current thread
int  sched_started(void);
int  sched_tick(void);                                           // per-tick: wake sleepers + slice
void sleep_ticks(uint64_t ticks);                                // block for N ticks
void sleep_ms(uint64_t ms);                                      // block for N ms (1 ms == 1 tick)

// Length of a thread's time slice, in timer ticks (Linux-style quantum).
#define SCHED_TIME_SLICE 10

// Critical-section helpers: mask IRQs so the timer can't corrupt the run-queue
// while a thread modifies it. irq_save() returns the old DAIF for irq_restore().
static inline uint64_t irq_save(void)
{
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");  // set the I (IRQ) mask bit
    return daif;
}

static inline void irq_restore(uint64_t daif)
{
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}
```

- [ ] **Step 2: Update `thread_create` (priority + IRQ-safe) in `src/sched.c`**

Replace the existing `thread_create` with:

```c
struct thread *thread_create(void (*fn)(void *), void *arg, int priority)
{
    struct thread *t = kmalloc(sizeof(struct thread));
    uint8_t *stack   = kmalloc(STACK_SIZE);

    uint8_t *c = (uint8_t *)&t->ctx;
    for (unsigned i = 0; i < sizeof(t->ctx); i++) {
        c[i] = 0;
    }

    t->stack     = stack;
    t->state     = THREAD_RUNNABLE;
    t->id        = next_id++;
    t->priority  = priority;
    t->wake_tick = 0;
    t->next      = 0;

    t->ctx.sp  = (uint64_t)(uintptr_t)(stack + STACK_SIZE);
    t->ctx.lr  = (uint64_t)(uintptr_t)thread_trampoline;
    t->ctx.x19 = (uint64_t)(uintptr_t)fn;
    t->ctx.x20 = (uint64_t)(uintptr_t)arg;

    // Link at the ring tail (just before current). IRQ-masked: the timer must
    // not walk a half-updated ring.
    uint64_t flags = irq_save();
    if (current) {
        struct thread *tail = current;
        while (tail->next != current) {
            tail = tail->next;
        }
        tail->next = t;
        t->next = current;
    } else {
        t->next = t;   // no scheduler yet (only happens in the create-only test)
    }
    irq_restore(flags);
    return t;
}
```

- [ ] **Step 3: Wrap `yield`/`thread_exit` in IRQ-masked sections in `src/sched.c`**

Replace `yield` and `thread_exit` with:

```c
void yield(void)
{
    uint64_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void thread_exit(void)
{
    uint64_t flags = irq_save();
    current->state = THREAD_EXITED;   // tombstone; schedule() skips it
    schedule();                       // switch away; never returns here
    irq_restore(flags);               // unreachable
    for (;;) { }
}
```

(Also set `boot_thread.priority` — add `boot_thread.priority = 0;` inside
`sched_init`, after `boot_thread.id = 0;`, so the field is initialized. The
`schedule()` body is unchanged in this task.)

- [ ] **Step 4: Update `thread_create` call sites**

In `src/tests.c`:
- `test_thread_create_context`: `thread_create(noop_thread, arg)` →
  `thread_create(noop_thread, arg, 1)`.
- `test_round_robin_order`: both calls gain `, 1` →
  `thread_create(rr_worker, (void *)(uintptr_t)1, 1)` and
  `thread_create(rr_worker, (void *)(uintptr_t)2, 1)`.

In `src/kmain.c`, the three demo creates gain `, 1`:
```c
    thread_create(demo_thread, (void *)(uintptr_t)'A', 1);
    thread_create(demo_thread, (void *)(uintptr_t)'B', 1);
    thread_create(demo_thread, (void *)(uintptr_t)'C', 1);
```

- [ ] **Step 5: Build and confirm existing tests still pass (no behavior change)**

Run:
```bash
make test 2>&1 | grep -E "passed|FAIL"
```
Expected: all 9 tests still pass; exit 0. (IRQ masking during tests is a no-op
since interrupts are already off there.)

- [ ] **Step 6: Commit**

```bash
git add src/sched.h src/sched.c src/tests.c src/kmain.c
git commit -m "refactor: thread priority field + IRQ-safe scheduler sections"
```

---

## Task 2: Strict-priority scheduling + idle thread (TEST FIRST)

**Files:**
- Modify: `src/tests.c`
- Modify: `src/sched.c`

- [ ] **Step 1: Write the failing priority test in `src/tests.c`**

Add before the `tests[]` array:

```c
static int pri_log[8];
static int pri_n;
static void pri_hi(void *a) { (void)a; pri_log[pri_n++] = 2; }  // returns -> exits
static void pri_lo(void *a) { (void)a; pri_log[pri_n++] = 1; }

static void test_priority_order(void)
{
    pmm_init();
    kheap_init();
    pri_n = 0;

    sched_init();
    thread_create(pri_lo, 0, 1);   // LOW priority, created FIRST
    thread_create(pri_hi, 0, 2);   // HIGH priority, created SECOND

    while (pri_n < 2) {            // idle thread yields until both have run
        yield();
    }

    // Strict priority: the high-priority thread runs first, despite being
    // created second (round-robin alone would give 1,2).
    KASSERT(pri_log[0] == 2);
    KASSERT(pri_log[1] == 1);
}
```

Register it after the time-slice test in `tests[]`:

```c
    { "sched: priority order",            test_priority_order },
```

- [ ] **Step 2: Run it and watch it FAIL (RED)**

Run:
```bash
make test 2>&1 | grep -E "priority order|assertion|passed|FAIL"
```
Expected: `[FAIL] sched: priority order` with an assertion failure (plain
round-robin ran the low-priority thread first → `pri_log[0] == 1`). Non-zero exit.

- [ ] **Step 3: Make the scheduler priority-aware + add the idle thread in `src/sched.c`**

Replace `sched_init` so the boot context becomes the idle thread:

```c
void sched_init(void)
{
    boot_thread.stack     = 0;                 // it already has the kernel boot stack
    boot_thread.state     = THREAD_RUNNING;
    boot_thread.id        = 0;
    boot_thread.priority  = -1;                // idle: below any created thread
    boot_thread.wake_tick = 0;
    boot_thread.next      = &boot_thread;      // a ring of one
    current  = &boot_thread;
    next_id  = 1;
    started  = 1;
    slice_left = SCHED_TIME_SLICE;
}
```

Replace `schedule` with the priority scan:

```c
// Pick the highest-priority RUNNABLE thread, round-robin within a level, and
// switch to it. The idle thread is always RUNNABLE at the lowest priority, so a
// runnable thread always exists.
void schedule(void)
{
    // current yields the CPU unless the caller already parked it (SLEEPING/EXITED).
    if (current->state == THREAD_RUNNING) {
        current->state = THREAD_RUNNABLE;
    }

    // Scan the whole ring starting just after current. "Strictly greater" means
    // among equal top priorities we keep the FIRST one after current -> round
    // robin within the level.
    struct thread *best = 0;
    struct thread *t = current->next;
    do {
        if (t->state == THREAD_RUNNABLE) {
            if (!best || t->priority > best->priority) {
                best = t;
            }
        }
        t = t->next;
    } while (t != current->next);

    best->state = THREAD_RUNNING;
    if (best == current) {
        return;                      // current is still the best -- keep running
    }

    struct thread *prev = current;
    current = best;
    slice_left = SCHED_TIME_SLICE;   // fresh slice for the newly-running thread
    cpu_switch(&prev->ctx, &best->ctx);
}
```

- [ ] **Step 4: Run it and watch it PASS (GREEN)**

Run:
```bash
make test 2>&1 | grep -E "priority order|round-robin|passed|FAIL"
```
Expected: `[PASS] sched: priority order`, `[PASS] sched: round-robin order`
(equal priorities still round-robin), and all tests passing; exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/tests.c src/sched.c
git commit -m "feat: strict-priority scheduling + idle thread (test-first)"
```

---

## Task 3: Sleep + timer wakeup (TEST FIRST)

**Files:**
- Modify: `src/tests.c`
- Modify: `src/sched.c`

- [ ] **Step 1: Write the failing sleep test in `src/tests.c`**

Add before the `tests[]` array:

```c
static char slp_log[8];
static int slp_n;
static void slp_worker(void *a)
{
    (void)a;
    slp_log[slp_n++] = 'S';
    sleep_ticks(3);
    slp_log[slp_n++] = 'W';
}

static void test_sleep_wakes_after_ticks(void)
{
    pmm_init();
    kheap_init();
    slp_n = 0;

    sched_init();                          // jiffies := 0
    thread_create(slp_worker, 0, 1);       // priority above idle

    yield();                               // run worker: logs 'S', sleeps 3 ticks
    KASSERT(slp_n == 1);
    KASSERT(slp_log[0] == 'S');

    sched_tick(); yield(); KASSERT(slp_n == 1);   // tick 1: still asleep
    sched_tick(); yield(); KASSERT(slp_n == 1);   // tick 2: still asleep
    sched_tick(); yield();                        // tick 3: wakes
    KASSERT(slp_n == 2);
    KASSERT(slp_log[1] == 'W');
}
```

Register it after the priority test in `tests[]`:

```c
    { "sched: sleep wakes after ticks",   test_sleep_wakes_after_ticks },
```

- [ ] **Step 2: Run it and watch it FAIL (RED)**

Run:
```bash
make test 2>&1 | grep -iE "undefined reference|Error" | head -3
```
Expected: `undefined reference to 'sleep_ticks'`; non-zero exit. RED.

- [ ] **Step 3: Implement sleep + jiffies + wakeup in `src/sched.c`**

Add a `jiffies` clock near the other statics (after `static int slice_left ...`):

```c
static uint64_t jiffies;   // ticks since sched_init (the sleep clock)
```

Set it to 0 in `sched_init` — add `jiffies = 0;` after `slice_left = SCHED_TIME_SLICE;`.

Add the sleep functions (e.g. after `thread_exit`):

```c
// Block the current thread for `ticks` timer ticks, then become runnable again.
void sleep_ticks(uint64_t ticks)
{
    uint64_t flags = irq_save();
    current->wake_tick = jiffies + ticks;
    current->state = THREAD_SLEEPING;   // schedule() will skip us
    schedule();                         // switch away; returns once we're woken
    irq_restore(flags);
}

void sleep_ms(uint64_t ms)
{
    sleep_ticks(ms);   // TIMER_HZ == 1000, so 1 ms == 1 tick
}
```

Replace `sched_tick` so it advances `jiffies` and wakes sleepers:

```c
int sched_tick(void)
{
    if (!started) {
        return 0;
    }
    jiffies++;

    // Wake any sleeper whose deadline has arrived. If a woken thread outranks
    // the current one, ask for an immediate reschedule so priority is prompt.
    int wake_preempt = 0;
    struct thread *t = current;
    do {
        if (t->state == THREAD_SLEEPING && t->wake_tick <= jiffies) {
            t->state = THREAD_RUNNABLE;
            if (t->priority > current->priority) {
                wake_preempt = 1;
            }
        }
        t = t->next;
    } while (t != current);

    if (wake_preempt) {
        slice_left = SCHED_TIME_SLICE;
        return 1;
    }
    if (--slice_left <= 0) {
        slice_left = SCHED_TIME_SLICE;
        return 1;
    }
    return 0;
}
```

- [ ] **Step 4: Run it and watch it PASS (GREEN)**

Run:
```bash
make test 2>&1 | grep -E "sleep wakes|passed|FAIL"
```
Expected: `[PASS] sched: sleep wakes after ticks` and all tests passing; exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/tests.c src/sched.c
git commit -m "feat: sleep_ticks/sleep_ms with timer wakeup (test-first)"
```

---

## Task 4: Demo + notes

**Files:**
- Modify: `src/kmain.c`
- Modify: `docs/notes/phase-5.md`

- [ ] **Step 1: Replace the demo in `src/kmain.c`**

Replace the `demo_thread` function with two demo bodies:

```c
// High-priority thread: print a short burst, then sleep so lower-priority
// threads get the CPU. Demonstrates priority preemption + sleep.
static void hi_thread(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < 5; i++) {
            uart_putc('A');
            for (volatile int d = 0; d < 1000000; d++) { }
        }
        sleep_ms(40);   // yield the CPU to B/C for ~40 ms
    }
}

// Lower-priority thread: print its letter continuously. Two of these at equal
// priority round-robin while the high-priority thread sleeps.
static void lo_thread(void *arg)
{
    char c = (char)(uintptr_t)arg;
    for (;;) {
        uart_putc(c);
        for (volatile int d = 0; d < 1000000; d++) { }
    }
}
```

Replace the three `thread_create(demo_thread, ...)` lines (and the banner) with:

```c
    sched_init();                                       // boot thread becomes idle (prio -1)
    thread_create(hi_thread, 0, 2);                     // A: high priority
    thread_create(lo_thread, (void *)(uintptr_t)'B', 1);// B: low priority
    thread_create(lo_thread, (void *)(uintptr_t)'C', 1);// C: low priority
    kprintf("Scheduler: A=high (bursts then sleeps), B/C=low (round-robin).\n");
```

- [ ] **Step 2: Build and observe priority + sleep**

Run:
```bash
make clean && make run
```
Expected: bursts of `AAAAA` (high priority preempts), then stretches of `BCBCBC…`
(equal-priority round-robin) while `A` sleeps, repeating. Quit with `Ctrl-C`.
(Capture without starving QEMU, e.g. `… | head -c 1200`.)

- [ ] **Step 3: Confirm the suite is still green**

Run:
```bash
make test 2>&1 | grep -E "passed|FAIL"
```
Expected: all 11 tests pass; exit 0.

- [ ] **Step 4: Document in `docs/notes/phase-5.md`**

Append a "Scheduler refinement" section covering: the priority model (higher =
more important; strict + round-robin within a level); the idle thread (always
runnable, lowest priority); `sleep_ticks`/`sleep_ms` and the `jiffies` wakeup in
`sched_tick` (including wake-preemption); tombstone `EXITED` threads; and the
IRQ-masked critical sections (`irq_save`/`irq_restore`) that make the run-queue
race-free on a single core. Note the known leaks (exited thread structs/stacks)
and that starvation of low priority is by design (use `sleep`).

- [ ] **Step 5: Commit**

```bash
git add src/kmain.c docs/notes/phase-5.md
git commit -m "feat: priority+sleep scheduler demo; docs"
```

---

## Self-review

- **Spec coverage:** priority field + IRQ-safety + signature (T1); strict-priority `schedule` + idle thread (T2); `sleep_ticks`/`sleep_ms` + `jiffies` + `sched_tick` wakeup + wake-preempt (T3); demo + notes (T4). Tests: priority order (T2, test-first), sleep wakeup (T3, test-first); existing round-robin/time-slice kept green (T1/T2). All success criteria exercised.
- **Placeholder scan:** none — all code complete; RED steps are genuine (assertion for priority, link error for sleep).
- **Type consistency:** `struct context` offsets unchanged (switch.S still valid). `struct thread` new fields (`priority`,`wake_tick`) + `THREAD_SLEEPING` used consistently in `sched.c`/`tests.c`. `thread_create(fn,arg,priority)` updated at all call sites (T1.4, T2, T3, T4). `sleep_ticks`/`sleep_ms`/`sched_tick`/`schedule`/`sched_init` signatures match `sched.h` (T1) and definitions (T2/T3). `irq_save`/`irq_restore` defined in `sched.h` (T1), used in `sched.c` (T1/T3).
