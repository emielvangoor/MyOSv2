// gic.h -- interface to the GIC (Generic Interrupt Controller).
// The GIC is the hardware that routes interrupt signals from devices to the
// CPU. We tell it which interrupts to allow, ask it which one fired, and tell
// it when we've finished handling one.
#pragma once
#include <stdint.h>

void gic_init(void);              // enable the controller
void gic_enable_irq(uint32_t id); // allow a specific interrupt id through
uint32_t gic_ack(void);           // "what fired?" -- acknowledge & get its id
void gic_eoi(uint32_t id);        // "done with it" -- end-of-interrupt
