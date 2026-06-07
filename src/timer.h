#pragma once
#include <stdint.h>

void timer_init(void);        // start periodic (1 s) timer interrupts
void timer_handle_irq(void);  // called from irq_handler on each tick
uint64_t timer_ticks(void);   // number of ticks so far
