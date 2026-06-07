#include <stdint.h>
#include "timer.h"
#include "gic.h"
#include "kprintf.h"

#define TIMER_IRQ 30   // NS EL1 physical timer PPI on the GIC

static uint64_t interval;   // timer ticks per period (1 second)
static uint64_t ticks;      // number of periods elapsed

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void write_tval(uint64_t v)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void enable_timer(void)
{
    uint64_t ctl = 1;   // bit0 ENABLE=1, bit1 IMASK=0 -> running, unmasked
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(ctl));
}

void timer_init(void)
{
    interval = read_cntfrq();   // fire once per second
    write_tval(interval);
    enable_timer();
    gic_enable_irq(TIMER_IRQ);
}

void timer_handle_irq(void)
{
    write_tval(interval);       // re-arm for the next period
    ticks++;
    kprintf("tick %d\n", (int)ticks);
}

uint64_t timer_ticks(void)
{
    return ticks;
}
