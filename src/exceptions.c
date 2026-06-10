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
#include "syscall.h"
#include "vm.h"
#include "signal.h"
#include "console.h"
#include "net.h"
#include "input.h"

// GIC interrupt id of UART0 on the virt board: shared peripheral interrupt (SPI)
// 1, and SPIs start at id 32, so 32 + 1 = 33.
#define UART_IRQ 33

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

    // EC 0x25 = data abort taken from EL1: the KERNEL touched a bad page. The
    // common legitimate case is the kernel writing into a user buffer (e.g.
    // sys_read storing a byte) that became copy-on-write after a fork. Treat it
    // exactly like a user COW fault: copy the page and retry the instruction.
    if (ec == 0x25 && (esr & (1u << 6))) {   // WnR=1 -> it was a write
        uint64_t far;
        __asm__ volatile("mrs %0, far_el1" : "=r"(far));
        if (sched_current_as() && cow_fault(sched_current_as(), far)) {
            return;                          // page copied; eret retries the store
        }
    }

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
    // Ask the interrupt controller which interrupt fired (and acknowledge it).
    uint32_t id = gic_ack();

    int resched = 0;
    if (id == 30) {            // 30 = the physical timer's interrupt id
        timer_handle_irq();    // heartbeat: re-arm + count this tick
        resched = sched_tick(); // 1 only when the current thread's slice expired
    } else if (id == UART_IRQ) {
        // Console input arrived: the line discipline queues it (or turns Ctrl-C
        // into a SIGINT) and wakes whatever reader is blocked in console_getc().
        console_isr();
    } else if ((int)id == net_irq_id()) {
        // The NIC received a frame (or finished a transmit): acknowledge the
        // device and wake any thread blocked waiting for a packet.
        net_isr();
    } else if ((int)id == input_irq_id(0) || (int)id == input_irq_id(1)) {
        // A key or pointer event arrived: acknowledge and wake readers; the
        // event itself is drained by the woken input_read syscall.
        input_isr();
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

    // If this IRQ interrupted user mode (SPSR.M == EL0t), deliver any signal now
    // pending on the (resumed) current thread before we return to it.
    if ((tf->spsr & 0xF) == 0) {
        signals_deliver(tf);
    }
}

// Synchronous exception from EL0 (user mode). The only expected cause is an
// `svc` (system call); anything else is a user fault, which we report and turn
// into a thread exit rather than crashing the kernel.
void el0_sync_handler(struct trapframe *tf)
{
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3f);

    if (ec == 0x15) {            // EC 0x15 = SVC executed in AArch64
        do_syscall(tf);
        signals_deliver(tf);     // deliver any pending signal as we return to EL0
    } else if (ec == 0x24) {     // EC 0x24 = data abort from a lower EL
        uint64_t far;
        __asm__ volatile("mrs %0, far_el1" : "=r"(far));
        // ESR bit 6 (WnR) = 1 means the access was a write. A write to a
        // read-only COW page means: make a private copy and retry.
        if ((esr & (1u << 6)) && cow_fault(sched_current_as(), far)) {
            return;              // handled -- eret retries the store
        }
        kprintf("User data abort at 0x%lx (ESR=0x%lx) -- killing thread.\n", far, esr);
        thread_exit(-1);             // killed by a fault
    } else {
        kprintf("User fault at EL0: EC=0x%x ELR=0x%lx ESR=0x%lx -- killing thread.\n",
                ec, tf->elr, esr);
        thread_exit(-1);             // killed by a fault
    }
}

// Anything we didn't expect (FIQ, SError, etc.). We can't sensibly continue, so
// report and halt.
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
