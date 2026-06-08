# MyOSv2 — Phase 6a Design (User Mode EL0 + System Calls)

**Date:** 2026-06-08
**Status:** Approved

## Goal

Run a thread at **EL0** (unprivileged) and let it reach the kernel only through
**system calls** (`svc`). First of two milestones toward full per-process
isolation (6b adds per-process page tables). Shared address space for now.

Builds on Phase 2 (exception vectors / sync path), Phase 3 (MMU), Phase 5 +
scheduler refinement (threads, sleep). Developed test-first; the syscall
dispatch is heavily unit-tested.

## Entering EL0

A user thread is created like a kernel thread, but its initial trampoline drops
to EL0 instead of running a function at EL1:
- `enter_user(entry, user_sp)` (assembly): `msr sp_el0, user_sp`;
  `msr elr_el1, entry`; `msr spsr_el1, #0` (M=EL0t, DAIF=0 → IRQs enabled);
  `eret`.
- `user_entry_trampoline` (assembly) is the thread's initial `lr`: it reads the
  user entry (`x19`) and user stack (`x20`) restored by `cpu_switch`, then
  `bl enter_user` (never returns).

Each user thread has **two stacks**: its `kmalloc` stack is the **kernel stack**
(`SP_EL1`, used when it traps); a second `kmalloc` stack is the **user stack**
(`SP_EL0`). On an exception from EL0 the CPU auto-switches to `SP_EL1`.

### SP_EL0 must be saved/restored per thread (correctness)

`SP_EL0` is a single banked register shared by all EL0 execution. With more than
one user thread, a resumed user thread would otherwise run on whatever user stack
the *last* EL0 thread used. Fix: `kernel_entry`/`kernel_exit` save/restore
`SP_EL0` in the trap frame. The trap frame gains an `sp_el0` field at offset 264
(the existing 8-byte pad in the 272-byte frame — size unchanged):

```c
struct trapframe {
    uint64_t x[31];   // 0..240
    uint64_t elr;     // 248
    uint64_t spsr;    // 256
    uint64_t sp_el0;  // 264
};
```

## Vector table: activate the EL0 entries

Phase 2 stubbed the "lower EL, AArch64" vector group with `default_handler`. Now:
- `+0x400` (sync, lower EL) → `el0_sync`.
- `+0x480` (IRQ, lower EL) → the existing `el1_irq` path (so the timer preempts
  EL0 threads like any other).

`el0_sync(tf)`: read `ESR_EL1`; if exception class is `SVC` (EC `0x15`), call
`do_syscall(tf)`; otherwise it's a user fault → report and `thread_exit()` (kill
the offending thread, don't crash the kernel).

## Syscall ABI & dispatch

Linux-style AArch64: number in **`x8`**, args in `x0–x5`, return in `x0`. The user
runs `svc #0`. `do_syscall(struct trapframe *tf)` reads `tf->x[8]`, dispatches,
and writes the result into `tf->x[0]` (so `eret` returns it). Syscalls:

| # | name | action | return |
|---|------|--------|--------|
| 0 | `SYS_WRITE` | print `x1` bytes of the string at `x0` to the UART | bytes written |
| 1 | `SYS_GETPID` | — | `current` thread id |
| 2 | `SYS_YIELD` | cooperative reschedule | 0 |
| 3 | `SYS_SLEEP` | `sleep_ms(x0)` | 0 |
| 4 | `SYS_EXIT` | `thread_exit()` (no return) | — |
| other | — | unknown number | `-1` |

`do_syscall` is plain C operating on a `struct trapframe`, so it is fully
**unit-testable** by constructing a fake frame.

## The user program

`user_main()` (compiled in the kernel for 6a) talks to the kernel **only** via a
`syscall()` inline-asm stub (`svc #0`). It: `SYS_WRITE` a greeting,
`SYS_GETPID`, `SYS_SLEEP`, `SYS_WRITE` again, `SYS_EXIT`. No privileged
instruction or direct hardware access — it is genuinely unprivileged. Reaching
`el0_sync` (the *lower-EL* vector) at all proves the `svc` came from EL0.

## MMU tweak (temporary, 6a only)

EL0 currently can't touch any RAM (`AP=00`, EL1-only). For 6a's shared address
space, relax **Normal RAM** blocks to `AP=01` (EL1+EL0 read/write), left
executable, so EL0 threads can run and use stacks. Device memory stays EL1-only.
**This removes kernel memory protection** — restored properly in Phase 6b via
per-process page tables with EL0-only user pages.

## Robustness

`schedule()` gains a guard: if no RUNNABLE thread is found (should never happen
while the idle thread exists), keep `current` running instead of dereferencing a
null `best`.

## Testing (test-first, many cases)

`do_syscall` is unit-tested before it exists (RED = undefined `do_syscall`):

1. `test_syscall_write_returns_len` — `SYS_WRITE` with len N returns N.
2. `test_syscall_getpid` — a worker's `SYS_GETPID` returns that worker's id.
3. `test_syscall_yield` — `SYS_YIELD` returns 0 (no corruption).
4. `test_syscall_unknown` — an unknown number returns `-1`.
5. `test_syscall_sleep_blocks` — `SYS_SLEEP(3)` from a worker blocks exactly 3
   ticks (the test drives `sched_tick`), then resumes.
6. `test_syscall_exit_ends_thread` — `SYS_EXIT` ends the thread (code after the
   syscall is never reached).
7. `test_syscall_return_in_x0` — the result is written to `tf->x[0]`.

Entering EL0 and the `svc` round-trip are verified by **observation** in the demo
(user output appears, interleaved with a kernel thread). All prior tests stay
green.

## Demo (`kmain`)

Launch a user thread (`user_main`) alongside a kernel thread that prints a letter.
Observe the user greeting (printed via the `write` syscall), the user sleeping and
waking, and the two preempted by the timer — a user program and kernel code
coexisting, the user reaching the kernel only through syscalls.

## Files & changes

| File | Responsibility |
|------|----------------|
| `src/syscall.h` / `syscall.c` | syscall numbers + `do_syscall()` |
| `src/user.h` / `user.c` | `user_main()` + the EL0 `syscall()` stub |
| `src/usermode.S` | `enter_user(entry, sp)` + `user_entry_trampoline` |
| `src/sched.h` / `sched.c` | `thread_create_user(...)`, `sched_current_id()`, `schedule` null-guard |
| `src/exceptions.h` | trap frame gains `sp_el0` |
| `src/vectors.S` | save/restore `SP_EL0`; route `el0_sync` / lower-EL IRQ |
| `src/exceptions.c` | `el0_sync` (SVC → `do_syscall`; else fault) |
| `src/mmu.c` | Normal RAM `AP=01` (EL0-accessible) |
| `src/kmain.c` | launch the user thread + a kernel thread |
| `docs/notes/phase-6a.md` | document EL0 entry, syscalls, SP_EL0 banking |

All new code heavily commented (project preference).

## Success criteria

- The user program prints `Hello from EL0 user mode!` via the `SYS_WRITE`
  syscall; `SYS_SLEEP`/`SYS_EXIT` work; it coexists with a kernel thread under
  timer preemption.
- 7 new `do_syscall` unit tests pass (after RED→GREEN); all prior tests green;
  `make test` exit 0; gate intact.

## Out of scope (→ Phase 6b)

Per-process page tables, TTBR0 switching, real memory protection, loading a
separately-linked user binary, signals, copy-from/to-user validation.
