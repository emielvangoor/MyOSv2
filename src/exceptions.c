#include <stdint.h>
#include "exceptions.h"
#include "kprintf.h"
#include "gic.h"
#include "timer.h"

extern char vector_table[];

void exc_init(void)
{
    __asm__ volatile("msr vbar_el1, %0" :: "r"(vector_table));
    __asm__ volatile("isb");   // ensure the new vector base is in effect
}

void sync_handler(struct trapframe *tf)
{
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3f);   // exception class

    kprintf("Caught sync exception: EC=0x%x, ELR=0x%lx, ESR=0x%lx\n",
            ec, tf->elr, esr);

    // Recover: skip the faulting instruction so eret resumes at the next one.
    tf->elr += 4;
}

void irq_handler(struct trapframe *tf)
{
    (void)tf;
    uint32_t id = gic_ack();
    if (id == 30) {            // timer
        timer_handle_irq();
    }
    gic_eoi(id);
}

void unhandled_exception(struct trapframe *tf)
{
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    kprintf("UNHANDLED exception: ELR=0x%lx ESR=0x%lx -- halting.\n",
            tf->elr, esr);
    for (;;) {
        __asm__ volatile("wfe");
    }
}
