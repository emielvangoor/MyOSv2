#include <stdint.h>
#include "uart.h"
#include "kprintf.h"
#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "mmu.h"

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
    kprintf("Hello, world from MyOSv2!\n");
    kprintf("Running at exception level EL%d.\n", (int)current_el());

    mmu_init();

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    kprintf("MMU enabled. SCTLR_EL1=0x%lx (M=%d).\n", sctlr, (int)(sctlr & 1));

    // Translation demo: VA 0x100000000 maps to PA 0x40200000.
    volatile uint32_t *va = (volatile uint32_t *)0x100000000UL;
    volatile uint32_t *pa = (volatile uint32_t *)0x40200000UL;
    *va = 0xDEADBEEF;
    __asm__ volatile("dsb sy");
    uint32_t got = *pa;
    kprintf("wrote 0x%x via VA 0x%lx, read 0x%x via PA 0x%lx -- %s\n",
            0xDEADBEEFU, 0x100000000UL, got, 0x40200000UL,
            got == 0xDEADBEEF ? "match!" : "MISMATCH");

    exc_init();
    gic_init();
    timer_init();
    enable_irqs();
    kprintf("Interrupts enabled; ticking under the MMU.\n");

    for (;;) {
        __asm__ volatile("wfi");
    }
}
