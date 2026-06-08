// kmain.c -- the kernel's C entry point and high-level startup sequence.
// =====================================================================
//
// boot.S sets up the stack and zeroes .bss, then calls kmain(). From here we
// bring the kernel to life in order: serial output, virtual memory, dynamic
// memory (this phase), then interrupts -- demonstrating each as we go.

#include <stdint.h>
#include "uart.h"
#include "kprintf.h"
#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "mmu.h"
#include "pmm.h"
#include "kheap.h"
#include "tests.h"
#include "semihost.h"
#include "sched.h"

// Read our exception level (privilege ring) from CurrentEL bits [3:2].
static uint64_t current_el(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (v >> 2) & 3;
}

// Unmask IRQs (clear PSTATE.I) so the timer interrupt can fire.
static inline void enable_irqs(void)
{
    __asm__ volatile("msr daifclr, #2");
}

// Demonstrate the Physical Memory Manager: distinct page-aligned addresses, and
// reuse of a freed page via the free list.
static void demo_pmm(void)
{
    void *p1 = pmm_alloc();
    void *p2 = pmm_alloc();
    void *p3 = pmm_alloc();
    kprintf("PMM: three pages -> 0x%lx 0x%lx 0x%lx (each 0x1000 apart)\n",
            (uint64_t)p1, (uint64_t)p2, (uint64_t)p3);

    pmm_free(p2);                 // hand the middle page back
    void *p4 = pmm_alloc();       // should be the same page again
    kprintf("PMM: freed middle page, next alloc -> 0x%lx (%s)\n",
            (uint64_t)p4, p4 == p2 ? "reused!" : "not reused");
}

// Demonstrate the heap: allocation, read/write, block reuse, and coalescing.
static void demo_heap(void)
{
    char *a = kmalloc(32);
    char *b = kmalloc(64);
    char *c = kmalloc(16);
    kprintf("heap: a=0x%lx b=0x%lx c=0x%lx\n",
            (uint64_t)a, (uint64_t)b, (uint64_t)c);

    // Write a string through `a` and read it back, proving the memory is usable.
    const char *msg = "kmalloc works";
    int i = 0;
    while (msg[i]) { a[i] = msg[i]; i++; }
    a[i] = '\0';
    kprintf("heap: wrote/read via a -> \"%s\"\n", a);

    // Reuse: free `b`, ask for the same size -> we get `b`'s address back.
    kfree(b);
    char *b2 = kmalloc(64);
    kprintf("heap: freed b, kmalloc(64) -> 0x%lx (%s)\n",
            (uint64_t)b2, b2 == b ? "reused!" : "not reused");

    // Coalescing: free two ADJACENT blocks (b2 then a). kfree merges them into
    // one chunk. A kmalloc(80) -- too big for either alone -- then fits in the
    // merged space at a's address, proving the merge happened.
    kfree(b2);
    kfree(a);
    char *big = kmalloc(80);
    kprintf("heap: freed a+b (merged); kmalloc(80) -> 0x%lx (%s)\n",
            (uint64_t)big, big == a ? "coalesced!" : "separate");

    kfree(c);
    kfree(big);
}

// A demo thread: print its letter forever with a crude delay. It never yields,
// so ONLY timer preemption can move the CPU to another thread -- which is exactly
// what we want to observe.
static void demo_thread(void *arg)
{
    char c = (char)(uintptr_t)arg;
    for (;;) {
        uart_putc(c);
        for (volatile int i = 0; i < 3000000; i++) {
            // burn time so each thread prints at a readable rate
        }
    }
}

void kmain(void)
{
    // --- 1. Serial output ---
    uart_init();
    kprintf("Hello, world from MyOSv2!\n");
    kprintf("Running at exception level EL%d.\n", (int)current_el());

    // --- 2. Virtual memory ---
    mmu_init();
    kprintf("MMU enabled.\n");

    // --- Self-tests: verify the foundations before doing anything else. ---
    // On a normal build, a failure halts the kernel (don't limp forward broken).
    // The `make test` build (-DTEST_EXIT) instead exits QEMU with a status code.
    int failed = run_self_tests();
#ifdef TEST_EXIT
    // `make test` build: report the result to the shell and stop.
    qemu_exit(failed == 0 ? 0 : 1);
#else
    // Normal build: a failure is fatal -- halt rather than run broken.
    if (failed) {
        kprintf("SELF-TESTS FAILED -- halting.\n");
        for (;;) {
            __asm__ volatile("wfi");
        }
    }
#endif

    // --- 3. Dynamic memory: page allocator, then the heap on top ---
    pmm_init();
    kheap_init();
    demo_pmm();
    demo_heap();

    // --- 4. Interrupts, then the scheduler + a preemption demo ---
    exc_init();
    gic_init();
    timer_init();

    sched_init();                                   // boot thread becomes thread 0
    thread_create(demo_thread, (void *)(uintptr_t)'A', 1);
    thread_create(demo_thread, (void *)(uintptr_t)'B', 1);
    thread_create(demo_thread, (void *)(uintptr_t)'C', 1);
    kprintf("Scheduler started: threads A/B/C, preempted by the timer.\n");

    enable_irqs();                                  // now the timer can preempt

    // The boot thread idles; preemption rotates A/B/C (and this idle thread).
    for (;;) {
        __asm__ volatile("wfi");
    }
}
