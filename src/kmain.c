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
#include "vfs.h"
#include "ramfs.h"
#include "initrd.h"
#include "proc.h"

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

// High-priority thread: print a short burst, then sleep so lower-priority
// threads get the CPU. Demonstrates priority preemption + sleep.
// Demonstrate the filesystem: read an initrd file, create+write+read a new file,
// and list the root directory.
static void demo_fs(void)
{
    vfs_mount_root(ramfs_type());
    initrd_unpack();

    struct file *h = vfs_open("/hello.txt");
    if (h) {
        char buf[32] = {0};
        int n = vfs_read(h, buf, sizeof(buf) - 1);
        buf[n] = '\0';
        kprintf("fs: /hello.txt = \"%s\"", buf);
        vfs_close(h);
    }

    vfs_create("/tmp.txt", VN_FILE);
    struct file *w = vfs_open("/tmp.txt");
    vfs_write(w, "written at runtime\n", 19);
    vfs_close(w);
    struct file *r = vfs_open("/tmp.txt");
    char b2[32] = {0};
    int m = vfs_read(r, b2, sizeof(b2) - 1);
    b2[m] = '\0';
    kprintf("fs: /tmp.txt = \"%s\"", b2);
    vfs_close(r);

    kprintf("fs: ls / ->");
    char name[32];
    for (int i = 0; vfs_readdir(vfs_root(), i, name) == 0; i++) {
        kprintf(" %s", name);
    }
    kprintf("\n");
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
    demo_fs();

    // --- 4. Interrupts, then the scheduler + a preemption demo ---
    exc_init();
    gic_init();
    timer_init();

    vm_init();
    sched_init();                                         // boot thread becomes idle (prio -1)
    proc_spawn("/bin/init", 2);                           // the shell, loaded from /bin/init
    kprintf("Starting the shell (loaded from /bin/init):\n");
    enable_irqs();                                  // now the timer can preempt

    // The boot thread idles; the scheduler runs the user + kernel threads.
    for (;;) {
        __asm__ volatile("wfi");
    }
}
