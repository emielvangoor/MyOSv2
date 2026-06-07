#include <stdint.h>
#include "uart.h"
#include "kprintf.h"
#include "exceptions.h"
#include "gic.h"
#include "timer.h"

static uint64_t current_el(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (v >> 2) & 3;
}

static inline void enable_irqs(void)
{
    __asm__ volatile("msr daifclr, #2");   // clear the I (IRQ) mask bit
}

void kmain(void)
{
    uart_init();
    kprintf("Hello, world from kernel!\n");
    kprintf("Running at exception level EL%d.\n", (int)current_el());

    exc_init();
    gic_init();
    timer_init();
    enable_irqs();
    kprintf("Interrupts enabled; timer running.\n");

    kprintf("Triggering a deliberate undefined instruction...\n");
    __asm__ volatile(".inst 0x00000000");   // udf #0 -> synchronous exception
    kprintf("Recovered from the fault; ticks should continue below.\n");

    for (;;) {
        __asm__ volatile("wfi");   // sleep until the next interrupt
    }
}
