# ARM64 Learning OS — Phase 2 Design (Exceptions & Interrupts)

**Date:** 2026-06-07
**Status:** Approved

## Goal

Give the kernel the ability to **stop and react to events** via the ARM64
exception mechanism. Two paths:

- **Asynchronous (IRQ):** the ARM generic timer fires periodically → the kernel
  prints `tick N` on its own while the main loop idles. (Foundation of multitasking.)
- **Synchronous (exception):** an instruction traps; we catch it, decode the
  cause, and recover. (Foundation of system calls and fault handling.)

Phase 2 stays at **EL1 with the MMU off**, building directly on Phase 0+1.

## Fixed technical decisions

| Decision | Choice | Notes |
|----------|--------|-------|
| Interrupt controller | GICv2 | QEMU `virt` default for cortex-a72; simpler than GICv3 |
| GIC distributor base | `0x08000000` | QEMU `virt` memory map |
| GIC CPU interface base | `0x08010000` | QEMU `virt` memory map |
| Timer | EL1 physical generic timer | `CNTP_*_EL0` system registers |
| Timer interrupt ID | 30 | PPI 14 (non-secure EL1 physical timer) on the GIC |
| Tick period | 1 second | `CNTP_TVAL_EL0 = CNTFRQ_EL0` per interval |
| Vector base | `VBAR_EL1` | 2 KiB-aligned table, 16 entries of 128 bytes |

## New components

| File | Responsibility |
|------|----------------|
| `src/vectors.S` | Exception vector table + low-level register save/restore (trap frame) |
| `src/exceptions.h` / `.c` | Install `VBAR_EL1`; C dispatch for sync vs IRQ; decode ESR/ELR/FAR |
| `src/gic.h` / `.c` | GICv2 driver: init distributor + CPU interface, enable/ack/EOI interrupts |
| `src/timer.h` / `.c` | Generic timer driver: read frequency, arm interval, handle tick |
| `src/kmain.c` | (modified) init vectors → GIC → timer → enable IRQs → fault demo → idle |

Each file has one clear responsibility, matching the existing Phase 0+1 style.

## The exception vector table

On an exception the CPU jumps to `VBAR_EL1 + offset`, where the offset encodes
the source/type. Table layout (16 entries × 128 bytes, 2 KiB-aligned):

```
+0x000  Sync   (current EL, SP_EL0)
+0x080  IRQ    (current EL, SP_EL0)
+0x100  FIQ    (current EL, SP_EL0)
+0x180  SError (current EL, SP_EL0)
+0x200  Sync   (current EL, SP_ELx)   <- our deliberate faults
+0x280  IRQ    (current EL, SP_ELx)   <- timer ticks
+0x300  FIQ    (current EL, SP_ELx)
+0x380  SError (current EL, SP_ELx)
+0x400..0x780  from lower EL (AArch64/AArch32) -- stubs for now (Phase 6)
```

Each entry saves a trap frame, calls a C handler, restores, and `eret`s.

### Trap frame

Saved on the stack on every exception, in this order (a C `struct trapframe`
mirrors it): general registers `x0`–`x30`, plus `ELR_EL1` (return address),
`SPSR_EL1` (saved processor state). Restored in reverse before `eret`.

### C dispatch

- `exc_init(void)` — set `VBAR_EL1` to the table address.
- `sync_handler(struct trapframe *tf)` — read `ESR_EL1` (decode EC field),
  `ELR_EL1`, `FAR_EL1`; print; for the demo, advance `tf->elr += 4` to skip the
  faulting instruction and recover.
- `irq_handler(struct trapframe *tf)` — call into the GIC to identify and
  dispatch the active interrupt.

## GICv2 driver

Two MMIO blocks:
- **Distributor** `GICD` at `0x08000000` — global enable + per-interrupt enable.
- **CPU interface** `GICC` at `0x08010000` — per-core acknowledge (`IAR`),
  priority mask (`PMR`), end-of-interrupt (`EOIR`).

API:
- `gic_init(void)` — enable distributor (`GICD_CTLR`), set `GICC_PMR = 0xFF`
  (accept all priorities), enable CPU interface (`GICC_CTLR`).
- `gic_enable_irq(uint32_t id)` — set the bit in `GICD_ISENABLER`.
- `gic_ack(void) -> uint32_t` — read `GICC_IAR` (returns active interrupt id).
- `gic_eoi(uint32_t id)` — write `GICC_EOIR` (dismiss).

## Generic timer driver

- `timer_init(void)` — read `CNTFRQ_EL0` (ticks/second), store interval =
  frequency (1 s), arm `CNTP_TVAL_EL0`, enable via `CNTP_CTL_EL0 = 1`, and
  `gic_enable_irq(30)`.
- `timer_handle_irq(void)` — reload `CNTP_TVAL_EL0` for the next interval,
  increment a `tick_count`, `kprintf("tick %d\n", tick_count)`.
- `timer_ticks(void) -> uint64_t` — accessor for the count.

## End-to-end IRQ flow

```
timer counts to 0 -> GIC raises IRQ 30 -> CPU vectors to +0x280
  -> vectors.S saves trap frame -> irq_handler(tf)
       -> id = gic_ack()
       -> if id == 30: timer_handle_irq()
       -> gic_eoi(id)
  -> vectors.S restores trap frame -> eret (resume idle loop)
```

Interrupts start masked; `kmain` unmasks with `msr daifclr, #2`.

## Synchronous fault demo

After interrupts are live, `kmain` executes an undefined instruction
(`udf #0`, encoding `0x00000000`). The CPU traps to `+0x200`:

1. `sync_handler` reads `ESR_EL1`, extracts the exception class (EC, bits 26–31).
2. Prints the EC, a short description, and the faulting address from `ELR_EL1`.
3. Recovers: `tf->elr += 4` so `eret` resumes at the next instruction.

The kernel then continues; timer ticks keep printing — demonstrating both the
sync path and that exceptions are recoverable.

## Success criteria

- Terminal prints `tick 1`, `tick 2`, … ~once per second, driven solely by timer
  interrupts while `kmain` idles in `wfe`.
- The fault demo prints e.g. `Caught sync exception: EC=0x00 (unknown/illegal),
  ELR=0x...`, then ticks resume.
- `make debug` + GDB can break in `irq_handler` and hit it each second.

## Out of scope (later phases)

MMU / virtual memory (Phase 3), kernel heap (Phase 4), context switching /
scheduling (Phase 5), EL0 user mode + syscalls (Phase 6). The lower-EL vector
entries are present but stubbed until Phase 6.
