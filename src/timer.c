// timer.c -- driver for the ARM generic timer.
// ============================================
//
// Every AArch64 CPU has a built-in timer, controlled through system registers
// (no MMIO needed). It works as a countdown: you load a value, the timer
// decrements it every tick of a fixed-frequency counter, and when it reaches
// zero it raises an interrupt. We reload it each time to get a periodic signal.
//
// The registers we use (the EL1 *physical* timer):
//   CNTFRQ_EL0     -- the counter frequency in Hz (ticks per second), read-only
//   CNTP_TVAL_EL0  -- countdown value; writing N means "fire in N ticks"
//   CNTP_CTL_EL0   -- control: bit0 ENABLE, bit1 IMASK (interrupt mask)

#include <stdint.h>
#include "timer.h"
#include "gic.h"
#include "kprintf.h"

#define TIMER_IRQ 30   // the EL1 physical timer's interrupt id on the GIC

static uint64_t interval;   // ticks per period (one second's worth)
static uint64_t ticks;      // how many periods have elapsed

// Read the timer's frequency (ticks per second).
static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

// Arm the countdown: "fire an interrupt after v ticks from now".
static inline void write_tval(uint64_t v)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

// Enable the timer with its interrupt unmasked.
static inline void enable_timer(void)
{
    uint64_t ctl = 1;   // bit0 ENABLE=1, bit1 IMASK=0 -> running and able to interrupt
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(ctl));
}

void timer_init(void)
{
    interval = read_cntfrq();   // one second = 'frequency' ticks
    write_tval(interval);       // schedule the first interrupt one second out
    enable_timer();             // start counting
    gic_enable_irq(TIMER_IRQ);  // allow the timer's interrupt through the GIC
}

// Called from irq_handler each time the timer fires.
void timer_handle_irq(void)
{
    write_tval(interval);       // re-arm for the next second (this also clears
                                //   the timer's pending condition)
    ticks++;
    kprintf("tick %d\n", (int)ticks);
}

uint64_t timer_ticks(void)
{
    return ticks;
}
