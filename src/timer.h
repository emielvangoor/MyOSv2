// timer.h -- interface to the ARM generic timer driver.
// The timer fires a periodic interrupt, giving the kernel a regular "heartbeat"
// (here, once per second). This heartbeat is what later enables preemptive
// multitasking -- forcibly switching tasks on each tick.
#pragma once
#include <stdint.h>

void timer_init(void);       // start the periodic timer interrupt
void timer_handle_irq(void); // called from irq_handler when the timer fires
uint64_t timer_ticks(void);  // how many ticks have elapsed so far
