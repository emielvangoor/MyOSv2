# MyOSv2 — Linux Tick + Time-Slice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Separate the timer tick (raise to 1000 Hz) from the scheduling quantum (a 10-tick time slice), so threads are preempted every ~10 ms instead of every tick.

**Architecture:** A fast 1 kHz timer heartbeat drives a per-thread `slice_left` counter in the scheduler; `sched_tick()` decrements it and signals a reschedule only when the slice expires. `irq_handler` switches only on that signal.

**Tech Stack:** Freestanding C, the Phase 2 timer/GIC path, the Phase 5 scheduler.

**Note (test-first):** `sched_tick()` is deterministic and is unit-tested before it exists (RED = linker `undefined reference`). The 1 kHz rate and finer interleaving are observed in the demo. `make test` must stay green; the gate is active. All code heavily commented.

---

## File structure

| File | Change |
|------|--------|
| `src/sched.h` | add `SCHED_TIME_SLICE` + `int sched_tick(void)` |
| `src/sched.c` | `slice_left`, `sched_tick`, reset on init/switch |
| `src/timer.c` | `TIMER_HZ = 1000`; interval = `CNTFRQ_EL0 / TIMER_HZ` |
| `src/exceptions.c` | preempt only when `sched_tick()` returns 1 |
| `src/tests.c` | `test_time_slice_expiry` (written first) |
| `docs/notes/phase-5.md` | tick-vs-slice section |

---

## Task 1: Time-slice logic (TEST FIRST)

**Files:**
- Modify: `src/sched.h`
- Modify: `src/tests.c`
- Modify: `src/sched.c`

- [ ] **Step 1: Declare the interface in `src/sched.h`**

Add the constant and prototype (this is the interface the test compiles against;
the implementation comes in Step 4). Insert after the existing prototypes:

```c
// Length of a thread's time slice, in timer ticks (Linux-style quantum).
#define SCHED_TIME_SLICE 10

// Called once per timer tick. Returns 1 when the current thread's time slice has
// expired (a reschedule is due) and resets the slice; returns 0 otherwise (or if
// the scheduler hasn't started).
int sched_tick(void);
```

- [ ] **Step 2: Write the failing test in `src/tests.c`**

Add before the `tests[]` array:

```c
static void test_time_slice_expiry(void)
{
    sched_init();   // resets the slice counter to a full SCHED_TIME_SLICE

    // The first SCHED_TIME_SLICE-1 ticks do NOT expire the slice.
    for (int i = 0; i < SCHED_TIME_SLICE - 1; i++) {
        KASSERT(sched_tick() == 0);
    }
    // The SCHED_TIME_SLICE-th tick expires it -> reschedule signal.
    KASSERT(sched_tick() == 1);

    // ...and it resets: the same pattern repeats.
    for (int i = 0; i < SCHED_TIME_SLICE - 1; i++) {
        KASSERT(sched_tick() == 0);
    }
    KASSERT(sched_tick() == 1);
}
```

Register it after the round-robin test in `tests[]`:

```c
    { "sched: time slice expiry",         test_time_slice_expiry },
```

- [ ] **Step 3: Run it and watch it FAIL (RED)**

Run:
```bash
make test 2>&1 | grep -iE "undefined reference|Error" | head -3
```
Expected: `undefined reference to 'sched_tick'`; non-zero exit. RED.

- [ ] **Step 4: Implement `sched_tick` in `src/sched.c`**

Add the counter near the other file-scope statics (after `static int started;`):

```c
static int slice_left = SCHED_TIME_SLICE;   // ticks remaining for `current`
```

Add the function (e.g. after `sched_started`):

```c
// Advance the current thread's time slice by one tick. When it runs out, reset
// it and tell the caller (the timer IRQ) to reschedule. Decoupling this from the
// timer tick is the Linux model: a fast tick, a slower scheduling quantum.
int sched_tick(void)
{
    if (!started) {
        return 0;
    }
    if (--slice_left <= 0) {
        slice_left = SCHED_TIME_SLICE;
        return 1;       // slice used up -> preempt
    }
    return 0;
}
```

In `sched_init`, give the boot thread a fresh slice — add this line (after
`started = 1;`):

```c
    slice_left = SCHED_TIME_SLICE;
```

In `schedule`, reset the slice whenever we actually switch — add this line
immediately after the `if (next == prev) { return; }` guard and before
`current = next;`:

```c
    slice_left = SCHED_TIME_SLICE;   // the newly-running thread gets a full slice
```

- [ ] **Step 5: Run it and watch it PASS (GREEN)**

Run:
```bash
make test 2>&1 | grep -E "time slice|passed|FAIL"
```
Expected: `[PASS] sched: time slice expiry` and all tests passing; exit 0.

- [ ] **Step 6: Commit**

```bash
git add src/sched.h src/tests.c src/sched.c
git commit -m "feat: add time-slice counter (sched_tick), test-first"
```

---

## Task 2: 1000 Hz tick + slice-based preemption

**Files:**
- Modify: `src/timer.c`
- Modify: `src/exceptions.c`

- [ ] **Step 1: Raise the timer to 1000 Hz in `src/timer.c`**

Add the rate constant near the top (after the `#define TIMER_IRQ 30` line):

```c
#define TIMER_HZ 1000   // timer interrupts per second (the Linux-style "tick")
```

In `timer_init`, change the interval from one second to one tick:

```c
    interval = read_cntfrq() / TIMER_HZ;   // fire every 1/TIMER_HZ second (1 ms)
```

(Replace the existing `interval = read_cntfrq();` line. `timer_handle_irq` is
unchanged.)

- [ ] **Step 2: Preempt only on slice expiry in `src/exceptions.c`**

Replace the body of `irq_handler` with:

```c
void irq_handler(struct trapframe *tf)
{
    (void)tf;  // we don't need the saved state here

    uint32_t id = gic_ack();
    int resched = 0;
    if (id == 30) {                // the timer
        timer_handle_irq();        // heartbeat: re-arm + count
        resched = sched_tick();    // 1 only when the time slice is used up
    }
    gic_eoi(id);                   // dismiss before switching, so the GIC is ready

    // Preempt only when the slice expired (sched_tick returns 0 if the scheduler
    // isn't running, so this is safe before sched_init).
    if (resched) {
        schedule();
    }
}
```

- [ ] **Step 3: Build and observe finer interleaving**

Run:
```bash
make clean && make run
```
Expected: after the startup output and `Scheduler started: ...`, the A/B/C
letters now interleave finely (short runs, e.g. `ABCABCAABCBC…`) instead of
~1-second blocks — the timer ticks at 1 kHz and threads switch every ~10 ms. Quit
with `Ctrl-C`.

- [ ] **Step 4: Confirm the suite is still green**

Run:
```bash
make test 2>&1 | grep -E "passed|FAIL"
```
Expected: all tests pass; exit 0. (The deterministic tests run before interrupts
are enabled, so the rate change doesn't affect them.)

- [ ] **Step 5: Commit**

```bash
git add src/timer.c src/exceptions.c
git commit -m "feat: 1000 Hz tick + 10ms time-slice preemption (Linux-style)"
```

---

## Task 3: Notes

**Files:**
- Modify: `docs/notes/phase-5.md`

- [ ] **Step 1: Add a tick-vs-slice section to `docs/notes/phase-5.md`**

Append a section explaining: the difference between the timer **tick** (now
1000 Hz / 1 ms) and the scheduling **quantum** (10 ticks / 10 ms); why Linux
decouples them (fast heartbeat for timekeeping/responsiveness, a larger slice to
limit switch overhead); how `sched_tick()` decrements `slice_left` and signals a
reschedule only on expiry; that `irq_handler` now switches only on that signal;
and how this changed the demo from per-second blocks to fine interleaving. Note
that real Linux uses variable, priority-based slices (CFS); we keep fixed
round-robin with the tick/slice split.

- [ ] **Step 2: Commit**

```bash
git add docs/notes/phase-5.md
git commit -m "docs: document Linux-style tick vs time-slice"
```

---

## Self-review

- **Spec coverage:** `SCHED_TIME_SLICE` + `sched_tick` declared (T1.1) and implemented with init/switch resets (T1.4); deterministic test written first (T1.2) with RED (T1.3) → GREEN (T1.5); `TIMER_HZ=1000` interval (T2.1); slice-based preemption in `irq_handler` (T2.2); interleaving observed (T2.3); suite green (T2.4); notes (T3). All success criteria exercised.
- **Placeholder scan:** none — all code complete; RED step is a genuine link error.
- **Type consistency:** `sched_tick(void)->int` and `SCHED_TIME_SLICE` consistent across `sched.h` (T1.1), `sched.c` (T1.4), `tests.c` (T1.2), `exceptions.c` (T2.2). `TIMER_HZ` local to `timer.c` (T2.1). `interval`/`read_cntfrq` match existing `timer.c`. `schedule`/`sched_init`/`sched_started`/`timer_handle_irq` signatures unchanged.
