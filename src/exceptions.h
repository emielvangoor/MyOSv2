#pragma once
#include <stdint.h>

// Saved CPU state on every exception. Field order MUST match vectors.S.
struct trapframe {
    uint64_t x[31];  // x0..x30   (offsets 0 .. 240)
    uint64_t elr;    // ELR_EL1   (offset 248) - return address
    uint64_t spsr;   // SPSR_EL1  (offset 256) - saved processor state
};

void exc_init(void);                              // set VBAR_EL1
void sync_handler(struct trapframe *tf);          // synchronous exceptions
void irq_handler(struct trapframe *tf);           // IRQs
void unhandled_exception(struct trapframe *tf);   // everything else
