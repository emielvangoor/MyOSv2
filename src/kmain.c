// kmain.c -- the kernel's C entry point and high-level startup sequence.
// =====================================================================
//
// boot.S sets up the stack and zeroes .bss, then calls kmain(). From here we
// bring the kernel to life in order: serial output, virtual memory, self-tests,
// dynamic memory, the filesystem, then interrupts + the scheduler, and finally
// hand control to the user-space shell.

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
#include "vfs.h"
#include "ramfs.h"
#include "initrd.h"
#include "proc.h"
#include "shm.h"
#include "block.h"

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

    // --- 4. Filesystem: mount ramfs as root and unpack the initrd into it ---
    // (the shell at /bin/init lives here, so this must happen before we spawn it)
    vfs_mount_root(ramfs_type());
    initrd_unpack();

    // --- Block device: probe the virtio-blk disk ---
    virtio_blk_init();
    kprintf("disk: %s\n", block_present() ? "virtio-blk ready" : "none");

    // --- 5. Interrupts, then the scheduler ---
    exc_init();
    gic_init();
    timer_init();

    vm_init();
    shm_init();                                           // shared-memory object table
    sched_init();                                         // boot thread becomes idle (prio -1)
    proc_spawn("/bin/init", 2);                           // the shell, loaded from /bin/init
    kprintf("Starting the shell (loaded from /bin/init):\n");
    enable_irqs();                                  // now the timer can preempt

    // The boot thread idles; the scheduler runs the user + kernel threads.
    for (;;) {
        __asm__ volatile("wfi");
    }
}
