# MyOSv2 — Linux-style Tick + Time-Slice Scheduling Design

**Date:** 2026-06-08
**Status:** Approved

## Goal

Adopt Linux's separation of the **timer tick** from the **scheduling quantum**:
raise the timer to a fixed 1000 Hz heartbeat (1 ms tick), and give each thread a
**time slice** of multiple ticks (10 ms) so it is preempted only when the slice
expires — not on every tick. Result: finer, more interleaved multitasking.

Small enhancement to the Phase 5 scheduler. Developed test-first (the slice logic
is deterministic).

## Parameters

- `TIMER_HZ = 1000` → timer interrupts every 1 ms (`interval = CNTFRQ_EL0 / TIMER_HZ`).
- `SCHED_TIME_SLICE = 10` ticks → each thread runs ~10 ms before preemption.

## Architecture change

Before: timer fires at 1 Hz and we preempt on every fire (tick == quantum).
After: timer fires at 1000 Hz (heartbeat); a separate per-thread time-slice
counter decides when to actually switch.

```
timer @ 1000 Hz  ->  tick++ , slice_left--
                       when slice_left reaches 0  ->  reset + preempt (schedule)
```

## Components

### `timer.c` — 1000 Hz tick
`timer_init` sets `interval = read_cntfrq() / TIMER_HZ` with `#define TIMER_HZ 1000`
(was `interval = read_cntfrq()`, i.e. 1 s). `timer_handle_irq` is unchanged
(re-arm + `ticks++`).

### `sched.c` — the time slice
- `#define`/constant `SCHED_TIME_SLICE` (10) exposed via `sched.h`.
- `static int slice_left;` initialized to `SCHED_TIME_SLICE`.
- `int sched_tick(void)` — called once per timer tick. If the scheduler is not
  started, return `0`. Otherwise decrement `slice_left`; if it reaches `<= 0`,
  reset to `SCHED_TIME_SLICE` and return `1` (reschedule due), else return `0`.
- `sched_init()` and `schedule()` reset `slice_left = SCHED_TIME_SLICE` (a
  newly-running thread gets a fresh quantum; `schedule()` resets only when it
  actually switches).

### `exceptions.c` — preempt on slice expiry
`irq_handler` becomes:
```c
uint32_t id = gic_ack();
int resched = 0;
if (id == 30) {
    timer_handle_irq();
    resched = sched_tick();   // returns 0 unless the slice expired (or sched not started)
}
gic_eoi(id);
if (resched) {
    schedule();
}
```
So 9 of every 10 ticks only advance the heartbeat; the 10th preempts.

## Testing (test-first)

`sched_tick()` is deterministic, so it is unit-tested before implementation:

`test_time_slice_expiry`:
```
sched_init();
for i in 0..SCHED_TIME_SLICE-2:  KASSERT(sched_tick() == 0)
KASSERT(sched_tick() == 1)                 // slice expired
for i in 0..SCHED_TIME_SLICE-2:  KASSERT(sched_tick() == 0)   // reset
KASSERT(sched_tick() == 1)
```

`SCHED_TIME_SLICE` is referenced by name (not a magic number). RED step is a
linker `undefined reference to 'sched_tick'`.

The 1000 Hz rate and finer interleaving are verified by observation in the demo,
not unit-tested (timing-dependent).

## Files & changes

| File | Change |
|------|--------|
| `src/sched.h` | add `SCHED_TIME_SLICE` constant + `int sched_tick(void)` |
| `src/sched.c` | `slice_left`, `sched_tick`, reset in `sched_init`/`schedule` |
| `src/timer.c` | `TIMER_HZ = 1000`; `interval = CNTFRQ_EL0 / TIMER_HZ` |
| `src/exceptions.c` | preempt only when `sched_tick()` returns 1 |
| `src/tests.c` | `test_time_slice_expiry` (written first) |
| `docs/notes/phase-5.md` | add a tick-vs-slice section |

All new code heavily commented (project preference).

## Success criteria

- New `[PASS] sched: time slice expiry` after a RED→GREEN cycle; all prior tests
  still pass; `make test` exits 0; commit gate intact.
- `make run` demo shows fine interleaving (e.g. `ABCABCABC…`) instead of big
  per-second blocks — the timer ticks at 1 kHz and threads switch every ~10 ms.

## Out of scope

Priority/variable-length slices (real CFS), nice values, tickless idle. We keep
fixed round-robin — just with the Linux tick/slice split.
