# Phase 6a notes — User Mode (EL0) + System Calls

## What changed

MyOSv2 now runs a thread at **EL0** (unprivileged). It can't touch hardware or
kernel-only memory directly; its only path into the kernel is a **system call**
(`svc`). Observed:

```
Hello from EL0 user mode!                  (user thread, via the write syscall)
kkkk...                                     (kernel thread runs while user sleeps)
user thread woke, exiting via syscall.      (user: sleep -> write -> exit)
```

## Exception levels

EL0 = user (least privileged), EL1 = kernel, EL2 = hypervisor, EL3 = firmware.
Until now everything ran at EL1. A user program runs at EL0: privileged
instructions and EL1-only memory are off-limits.

## Entering EL0

`enter_user(entry, user_sp)` (assembly) sets `SP_EL0`, `ELR_EL1 = entry`,
`SPSR_EL1 = 0` (M=EL0t, IRQs enabled), then `eret` — which lands at `entry`
running unprivileged. A user thread's initial `lr` is `user_entry_trampoline`,
which runs at EL1, reads the user entry/stack (stashed in x19/x20), and calls
`enter_user`.

## Two stacks per user thread

Each user thread has a **kernel stack** (`SP_EL1`, used when it traps) and a
**user stack** (`SP_EL0`). On an exception from EL0 the CPU auto-switches to
`SP_EL1`, so traps run on the kernel stack.

**SP_EL0 is banked and must be saved/restored.** It's a single register shared by
all EL0 execution, so `kernel_entry`/`kernel_exit` save/restore it in the trap
frame (new `sp_el0` field at offset 264 — the former 8-byte pad; frame stays 272
bytes). Without this, a resumed user thread would run on whatever stack the last
EL0 thread used.

## The syscall path

The "lower EL, AArch64" vector group (stubbed in Phase 2) is now live:
- `+0x400` (sync) → `el0_sync_handler`: if `ESR_EL1` exception class is `0x15`
  (SVC), call `do_syscall`; otherwise it's a user fault → report + `thread_exit`.
- `+0x480` (IRQ) → the normal IRQ path, so the timer preempts EL0 threads too.

Reaching `el0_sync_handler` *at all* proves the `svc` came from EL0 (the hardware
only routes to the lower-EL vectors for exceptions from a lower EL).

ABI (Linux-style AArch64): number in **x8**, args in **x0..x5**, return in **x0**.
`do_syscall(tf)` reads `tf->x[8]`, dispatches, writes the result to `tf->x[0]`.
Syscalls: `write`, `getpid`, `yield`, `sleep`, `exit` (unknown → `-1`).
`do_syscall` is plain C over a trap frame, so it's heavily unit-tested (7 cases).

## The EL0 RAM alias (and a hard-won lesson)

First attempt: map all RAM `AP=01` (EL0-accessible) for a shared address space.
That **hangs the moment the MMU turns on** — because ARMv8 forces any
**EL0-writable** page to be **Privileged-eXecute-Never (PXN)**. The kernel
executes from RAM at EL1, so making that RAM user-writable makes it
non-executable at EL1, and the first instruction fetch after enabling the MMU
faults (with no vectors yet → hang).

Fix: keep the kernel's identity mapping **EL1-only** (`AP=00`, executable), and
add a **second alias of the same RAM** at VA `0x80000000`–`0xC0000000` mapped
`AP=01` (EL0-accessible). User threads run at the alias (EL0); the kernel keeps
executing from its protected identity mapping (EL1). `user VA = physical +
USER_ALIAS_OFFSET (0x40000000)`. `thread_create_user` enters the user function
and user stack at their alias addresses. The kernel can still read user pointers
because the alias is EL1+EL0 (`AP=01`).

This is "shared address space, no real protection" — fine for 6a. Phase 6b
replaces the global alias with **per-process page tables** for true isolation.

## Testing

`do_syscall` is unit-tested (write/getpid/yield/unknown/sleep/exit/return-in-x0,
7 cases, test-first). Actually entering EL0 and the `svc` round-trip are verified
by observation in the demo.
