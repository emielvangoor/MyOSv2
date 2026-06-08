// exceptions.h -- exception/interrupt handling interface.
// =======================================================
//
// An "exception" on ARM is any event that makes the CPU stop its normal flow
// and jump to a handler: a fault (illegal instruction, bad memory access), an
// interrupt (IRQ from a device like the timer), or a system call. The CPU saves
// almost nothing automatically -- our assembly stub (vectors.S) must save the
// register state so the handler can run C, then restore it to resume cleanly.

#pragma once
#include <stdint.h>

// The saved CPU state pushed onto the stack on every exception.
//
// CRITICAL: the field order/offsets here MUST exactly match the store order in
// vectors.S's kernel_entry macro. The assembly builds this layout on the stack
// and passes a pointer to it; C then reads/writes it as this struct.
//   x[0..30] -> offsets 0..240
//   elr      -> offset 248  (ELR_EL1: the address to return to)
//   spsr     -> offset 256  (SPSR_EL1: saved processor state/flags)
struct trapframe {
    uint64_t x[31];  // general-purpose registers x0..x30
    uint64_t elr;    // where execution will resume on `eret`
    uint64_t spsr;   // saved PSTATE (condition flags, interrupt masks, etc.)
};

void exc_init(void);                            // install the vector table (set VBAR_EL1)
void sync_handler(struct trapframe *tf);        // handle synchronous exceptions
void irq_handler(struct trapframe *tf);         // handle IRQs (e.g. the timer)
void unhandled_exception(struct trapframe *tf); // catch-all for the rest
