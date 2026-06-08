// exceptions.c -- the C side of exception handling.
// =================================================
//
// vectors.S saves the CPU state and calls one of these functions. Here we
// decide what an exception means and what to do about it.

#include <stdint.h>
#include "exceptions.h"
#include "kprintf.h"
#include "gic.h"
#include "timer.h"
#include "sched.h"

// Defined in vectors.S: the start of our 16-entry vector table.
extern char vector_table[];

// Tell the CPU where our vector table is by writing its address to VBAR_EL1
// (Vector Base Address Register, EL1). After this, any exception jumps into
// our table instead of wherever it pointed before.
void exc_init(void)
{
    __asm__ volatile("msr vbar_el1, %0" :: "r"(vector_table));
    __asm__ volatile("isb");   // instruction barrier: make the change take effect
                               // before the next instruction runs
}

// Synchronous exceptions: caused directly BY an instruction (illegal opcode,
// bad memory access, a system call via `svc`, etc). "Synchronous" because the
// faulting instruction is exactly the one running.
void sync_handler(struct trapframe *tf)
{
    // ESR_EL1 = Exception Syndrome Register: tells us WHY we trapped.
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));

    // Bits [31:26] are the "exception class" (EC) -- the category of cause.
    // E.g. 0x00 = unknown/illegal instruction, 0x15 = SVC (syscall),
    // 0x24/0x25 = data abort (bad memory access).
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3f);

    // tf->elr is the address of the faulting instruction (from ELR_EL1).
    kprintf("Caught sync exception: EC=0x%x, ELR=0x%lx, ESR=0x%lx\n",
            ec, tf->elr, esr);

    // RECOVER: skip the faulting instruction by advancing the saved return
    // address by 4 bytes (one AArch64 instruction). When vectors.S does `eret`,
    // execution resumes at the NEXT instruction instead of re-faulting forever.
    // (The same idea -- adjusting ELR -- is how a syscall returns to user code.)
    tf->elr += 4;
}

// IRQ = Interrupt ReQuest: an asynchronous signal from a device (here, the
// timer). "Asynchronous" because it can arrive between any two instructions,
// unrelated to what the CPU was doing.
void irq_handler(struct trapframe *tf)
{
    (void)tf;  // we don't need the saved state for this simple handler

    // Ask the interrupt controller which interrupt fired (and acknowledge it).
    uint32_t id = gic_ack();

    int resched = 0;
    if (id == 30) {            // 30 = the physical timer's interrupt id
        timer_handle_irq();    // heartbeat: re-arm + count this tick
        resched = sched_tick(); // 1 only when the current thread's slice expired
    }

    // Tell the controller we're done so it can deliver the next one.
    gic_eoi(id);

    // Preempt only when the time slice is used up (Linux-style: a fast tick, a
    // larger scheduling quantum). sched_tick() returns 0 if the scheduler isn't
    // running yet, so this is safe before sched_init(). The switch happens on
    // this thread's own stack, so its in-progress trap frame travels with it.
    if (resched) {
        schedule();
    }
}

// Anything we didn't expect (FIQ, SError, exceptions from EL0 before Phase 6).
// We can't sensibly continue, so report and halt.
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
