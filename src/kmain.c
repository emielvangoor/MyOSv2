// kmain.c -- the kernel's C entry point and high-level startup sequence.
// =====================================================================
//
// boot.S finishes the low-level setup (stack, zeroed .bss) and calls kmain().
// From here on we're in C. This file orchestrates bringing the kernel to life:
// serial output, then virtual memory, then interrupts -- and demonstrates each.

#include <stdint.h>
#include "uart.h"
#include "kprintf.h"
#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "mmu.h"

// Read which exception level (privilege ring) we're running at.
// CurrentEL holds the level in bits [3:2]: EL0=user, EL1=kernel (us),
// EL2=hypervisor, EL3=firmware.
static uint64_t current_el(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (v >> 2) & 3;
}

// Unmask IRQs so the CPU will actually take timer interrupts.
// PSTATE.DAIF has mask bits for Debug, SError, IRQ, FIQ. Clearing the I bit
// (value 2) lets IRQs through. Until this runs, interrupts stay blocked.
static inline void enable_irqs(void)
{
    __asm__ volatile("msr daifclr, #2");
}

void kmain(void)
{
    // --- 1. Serial output up first, so we can print everything that follows ---
    uart_init();
    kprintf("Hello, world from MyOSv2!\n");
    kprintf("Running at exception level EL%d.\n", (int)current_el());

    // --- 2. Turn on virtual memory ---
    // After this, every address is translated through the page tables in mmu.c.
    // Because we identity-mapped our RAM and devices, the kernel keeps running
    // at the same addresses -- but now with the MMU (and caches) active.
    mmu_init();

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    kprintf("MMU enabled. SCTLR_EL1=0x%lx (M=%d).\n", sctlr, (int)(sctlr & 1));

    // --- 3. Prove translation is real ---
    // We mapped virtual 0x100000000 to physical 0x40200000. So writing through
    // the virtual pointer and reading through the physical pointer should see
    // the SAME memory -- two different addresses, one physical cell.
    volatile uint32_t *va = (volatile uint32_t *)0x100000000UL;  // virtual alias
    volatile uint32_t *pa = (volatile uint32_t *)0x40200000UL;   // its physical home
    *va = 0xDEADBEEF;
    __asm__ volatile("dsb sy");   // make sure the write has completed
    uint32_t got = *pa;
    kprintf("wrote 0x%x via VA 0x%lx, read 0x%x via PA 0x%lx -- %s\n",
            0xDEADBEEFU, 0x100000000UL, got, 0x40200000UL,
            got == 0xDEADBEEF ? "match!" : "MISMATCH");

    // --- 4. Turn on interrupts and start the timer ---
    exc_init();      // install the exception vector table (VBAR_EL1)
    gic_init();      // enable the interrupt controller
    timer_init();    // start the 1-second periodic timer interrupt
    enable_irqs();   // unmask IRQs so the timer can actually fire
    kprintf("Interrupts enabled; ticking under the MMU.\n");

    // --- 5. Idle ---
    // There's no work to do in the foreground. `wfi` (wait for interrupt) puts
    // the core to sleep until the next interrupt -- the timer -- wakes it. The
    // IRQ handler runs, prints a tick, and we return here to sleep again.
    for (;;) {
        __asm__ volatile("wfi");
    }
}
