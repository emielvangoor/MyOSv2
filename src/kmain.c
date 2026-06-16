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
#include "ext2.h"
#include "net.h"
#include "input.h"
#include "gfx.h"

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

    // --- Self-tests: verify the foundations before anything else. The disk/FS
    // tests that REFORMAT the disk are skipped on a normal boot (so persistent
    // data survives) but run under `make test` (-DTEST_EXIT), which also exits
    // QEMU with a status code. A normal boot halts on any failure.
    // --- FPU first: the scheduler now saves/restores V registers on every
    // switch, and the self-tests below exercise it -- FP instructions must
    // not trap from the very first context switch. ---
    fpu_enable();

    int failed = run_self_tests();
#ifdef TEST_EXIT
    qemu_exit(failed == 0 ? 0 : 1);
#else
    if (failed) {
        kprintf("SELF-TESTS FAILED -- halting.\n");
        for (;;) { __asm__ volatile("wfi"); }
    }
#endif

    // --- 3. Dynamic memory: page allocator, then the heap on top ---
    pmm_init();
    kheap_init();

    // --- 4. Filesystem: mount ramfs as root and unpack the initrd into it ---
    // (the shell at /bin/init lives here, so this must happen before we spawn it)
    vfs_mount_root(ramfs_type());
    initrd_unpack();

    // --- Block device + persistent /disk filesystem ---
    virtio_blk_init();
    if (block_present()) {
        // Mount the host-built ext2 image at /disk. A blank/zeroed disk (or a
        // non-ext2 image) fails the magic check; ext2_mount returns NULL and we
        // leave /disk unmounted (graceful) rather than mounting garbage.
        struct vnode *disk = ext2_mount();
        if (disk) {
            vfs_mount_at("/disk", disk);
            kprintf("disk: /disk mounted (ext2)\n");
        } else {
            kprintf("disk: present but not ext2 (unmounted)\n");
        }

        // TODO(ext2 Phase 2): re-enable the persistent /disk/boots boot counter
        // once the ext2 write path (create/write/truncate) lands. ext2 is
        // read-only in Phase 1, so the counter (which WRITES) is disabled. The
        // boot seed /init.l is only READ at boot, so the read path still drives
        // the frame autostart.
    } else {
        kprintf("disk: none\n");
    }

    // --- Network interface ---
    virtio_net_init();
    if (net_present()) {
        uint8_t m[6]; net_mac(m);
        kprintf("net: virtio-net %x:%x:%x:%x:%x:%x ready\n",
                m[0], m[1], m[2], m[3], m[4], m[5]);
        net_stack_init();                                 // bring up TCP/IP (ARP/IP/ICMP)
        // Lease an address via DHCP; if the server doesn't answer, keep the
        // built-in default so networking still works on QEMU's user-net.
        if (net_dhcp() == 0) {
            uint32_t ip = net_our_ip(), gw = net_gateway(), dns = net_dns();
            kprintf("net: DHCP leased %d.%d.%d.%d  gw %d.%d.%d.%d  dns %d.%d.%d.%d\n",
                    (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff,
                    (gw >> 24) & 0xff, (gw >> 16) & 0xff, (gw >> 8) & 0xff, gw & 0xff,
                    (dns >> 24) & 0xff, (dns >> 16) & 0xff, (dns >> 8) & 0xff, dns & 0xff);
        } else {
            uint32_t ip = net_our_ip();
            kprintf("net: DHCP failed, using %d.%d.%d.%d\n",
                    (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
        }
    } else {
        kprintf("net: none\n");
    }

    // --- Input devices (keyboard + tablet, for the graphical machine) ---
    input_init();
    kprintf(input_present() ? "input: keyboard + tablet ready\n" : "input: none\n");

    // --- Display (virtio-gpu scanout) ---
    gfx_init();
    if (gfx_present()) {
        // Activate the scanout NOW with a boot splash: without an active
        // scanout QEMU's window opens tiny showing "Display output is not
        // active" until something draws. Resource 99 = the splash; the first
        // display client's gfx_show replaces it.
        uint64_t fb = gfx_fb_new();
        if (fb && gfx_resource_setup(99, fb) == 0) {
            gfx_splash((uint32_t *)(uintptr_t)fb);
            gfx_show(99);
        }
        kprintf("gfx: virtio-gpu ready\n");
    } else {
        kprintf("gfx: none\n");
    }

    // --- 5. Interrupts, then the scheduler ---
    exc_init();
    gic_init();
    timer_init();
    gic_enable_irq(33);          // UART0 receive interrupt (interrupt-driven input)
    uart_rx_irq_enable();        // now that the GIC + handler are ready, arm RX
    if (net_present()) { gic_enable_irq(net_irq_id()); }   // NIC receive interrupt
    if (input_present()) {                                  // keyboard + tablet events
        gic_enable_irq(input_irq_id(0));
        gic_enable_irq(input_irq_id(1));
    }

    vm_init();
    shm_init();                                           // shared-memory object table
    sched_init();                                         // boot thread becomes idle (prio -1)
    proc_spawn("/bin/init", 2);                           // the shell, loaded from /bin/init
    kprintf("Starting the shell (loaded from /bin/init):\n");
    enable_irqs();                                  // now the timer can preempt
    sched_set_irqs_live(1);                         // blocking I/O may now sleep

    // The boot thread idles; the scheduler runs the user + kernel threads.
    for (;;) {
        __asm__ volatile("wfi");
    }
}
