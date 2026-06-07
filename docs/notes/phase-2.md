# Phase 2 notes — Exceptions & Interrupts

## What changed

The kernel went from a straight-line program to an **event-driven** one. It now
installs an exception vector table, catches and recovers from a synchronous
fault, and prints `tick N` once per second driven entirely by timer interrupts
(the main loop only does `wfi`).

## The exception vector table

On any exception the CPU jumps to `VBAR_EL1 + offset`. The table has **16
entries** in four groups of four (Sync / IRQ / FIQ / SError):

| Offset range | Source |
|--------------|--------|
| 0x000–0x180 | current EL, using SP_EL0 |
| 0x200–0x380 | current EL, using SP_ELx  ← where our faults & IRQs land |
| 0x400–0x580 | lower EL, AArch64 (used in Phase 6 for EL0) |
| 0x600–0x780 | lower EL, AArch32 |

Requirements: the table is 2 KiB-aligned (`.align 11`) and each entry sits on a
128-byte boundary (`.align 7`) — only 32 instructions fit per slot, so each slot
just branches to a handler.

## Trap frame (save/restore)

On entry we push all 31 general registers plus `ELR_EL1` (return address) and
`SPSR_EL1` (saved flags) onto the stack — 272 bytes, 16-aligned. `struct
trapframe` in C mirrors this exact layout, so the handler gets a typed view of
the saved state. On exit we restore everything and `eret`. This save/restore is
what makes an interrupt **invisible** to the interrupted code.

## Synchronous exceptions

- `ESR_EL1` = Exception Syndrome Register. Bits [31:26] are the **EC** (exception
  class). For our deliberate `udf #0` (encoding `0x00000000`) we observed
  `ESR=0x2000000` → `EC=0x0` ("Unknown reason" / illegal instruction); bit 25
  (IL) set means the instruction was 32-bit.
- `ELR_EL1` = the address that faulted (we observed `0x400801..`, the `udf`).
- `FAR_EL1` = faulting virtual address (for data/instruction aborts; not relevant
  for an illegal instruction).
- **Recovery:** we add 4 to the saved `ELR` so `eret` resumes at the *next*
  instruction. Same mechanism will later advance past an `svc` for system calls.

## Interrupt masking (DAIF)

IRQs start masked. `PSTATE.DAIF` has bits D(ebug) A(SError) I(RQ) F(IQ).
`msr daifclr, #2` clears the **I** bit (value 2) → IRQs now reach the CPU. That
single instruction is the moment the kernel becomes interrupt-driven.

## GICv2 (interrupt controller)

Two MMIO blocks on QEMU `virt`:
- **Distributor** `0x08000000` — global on/off and per-interrupt enable
  (`GICD_CTLR`, `GICD_ISENABLER`).
- **CPU interface** `0x08010000` — per-core: priority mask (`GICC_PMR`),
  acknowledge (`GICC_IAR`), end-of-interrupt (`GICC_EOIR`), enable (`GICC_CTLR`).

Handshake each IRQ: read `IAR` (acknowledge, get id) → handle → write `EOIR`
(dismiss). Minimal init that works on `virt`: `GICD_CTLR=1`, `GICC_PMR=0xFF`,
`GICC_CTLR=1`.

## ARM generic timer

Built into the CPU, programmed via system registers:
- `CNTFRQ_EL0` — counter frequency (ticks/second).
- `CNTP_TVAL_EL0` — countdown value; fires when it reaches 0. We reload it with
  the frequency each tick → 1 Hz.
- `CNTP_CTL_EL0` — bit0 ENABLE, bit1 IMASK. We write 1 (enabled, unmasked).
- Its interrupt is **PPI 14 → GIC interrupt id 30** (NS EL1 physical timer).

## End-to-end IRQ path

```
timer hits 0 -> GIC raises id 30 -> CPU vectors to VBAR+0x280 (el1_irq)
  -> save trap frame -> irq_handler(tf)
       id = gic_ack(); if id==30 timer_handle_irq(); gic_eoi(id)
  -> restore trap frame -> eret -> back to wfi
```

## Why `wfi` in the idle loop

`wfi` (wait for interrupt) puts the core to sleep until an interrupt is pending,
then the timer IRQ wakes it. After handling, `eret` returns into the loop and it
sleeps again. (`wfe` waits for events; `wfi` is the right primitive for "sleep
until the next interrupt".)

## Gotcha: GDB backtraces through exceptions

Our hand-written `vectors.S` emits no DWARF unwind info (CFI), so GDB can't
unwind past the exception boundary — `backtrace` loops on the `el1_irq` frame.
Cosmetic only; breakpoints and stepping inside handlers work fine. Could be fixed
later with `.cfi_*` directives or by chaining a frame record.

## Gotcha: GDB and the project .gdbinit in batch mode

`.gdbinit` auto-runs `target remote / break kmain / continue`. When scripting GDB
non-interactively, pass `-nx` to skip it and avoid double-connect errors.
