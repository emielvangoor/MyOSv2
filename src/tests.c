// tests.c -- characterization tests for the existing kernel subsystems.
// =====================================================================
//
// These lock in the behavior we verified by hand in Phase 4 so a future change
// can't silently break it. Each test re-initializes the allocators first so the
// tests are order-independent; re-init just resets bookkeeping pointers (any
// "leaked" pages are irrelevant -- there are ~244 MiB free).
//
// Going forward (Phase 5+) we work test-FIRST: add a failing KASSERT here, watch
// `make test` go red, then implement until green.

#include <stdint.h>
#include "tests.h"
#include "ktest.h"
#include "kprintf.h"
#include "pmm.h"
#include "kheap.h"
#include "sched.h"
#include "syscall.h"
#include "vm.h"
#include "vfs.h"
#include "ramfs.h"
#include "initrd.h"
#include "proc.h"
#include "elf.h"
#include "shm.h"
#include "pipe.h"
#include "signal.h"
#include "block.h"
#include "sfs.h"
#include "net.h"
#include "console.h"

#define PAGE 0x1000UL

// Disk-MUTATING tests (block writes, mkfs) reformat the persistent disk, so they
// run only in the `make test` build -- a normal boot skips them to keep its data.
// They begin with `if (!DISK_TESTS) return;` so they pass trivially when skipped.
#ifdef TEST_EXIT
#define DISK_TESTS 1
#else
#define DISK_TESTS 0
#endif

// --- PMM ---

static void test_pmm_aligned_and_contiguous(void)
{
    pmm_init();
    void *p1 = pmm_alloc();
    void *p2 = pmm_alloc();
    void *p3 = pmm_alloc();
    KASSERT(p1 != 0);
    KASSERT(((uint64_t)p1 & 0xFFF) == 0);             // page-aligned
    KASSERT((uint64_t)p2 - (uint64_t)p1 == PAGE);     // consecutive, 4 KiB apart
    KASSERT((uint64_t)p3 - (uint64_t)p2 == PAGE);
}

static void test_pmm_free_reuse(void)
{
    pmm_init();
    void *a = pmm_alloc();
    void *b = pmm_alloc();
    pmm_free(b);
    void *c = pmm_alloc();
    KASSERT(c == b);     // the freed page is handed back
    KASSERT(a != b);
}

static void test_pmm_alloc_pages_contiguous(void)
{
    pmm_init();
    void *run = pmm_alloc_pages(3);
    KASSERT(run != 0);
    KASSERT(((uint64_t)run & 0xFFF) == 0);
    void *next = pmm_alloc();
    KASSERT((uint64_t)next - (uint64_t)run == 3 * PAGE);   // run really was 3 pages
}

// --- Heap ---

static void test_kheap_write_read(void)
{
    pmm_init();
    kheap_init();
    int *p = kmalloc(sizeof(int) * 4);
    KASSERT(p != 0);
    KASSERT(((uint64_t)p & 0x7) == 0);     // 8-byte aligned
    for (int i = 0; i < 4; i++) {
        p[i] = i * 11;
    }
    KASSERT(p[0] == 0 && p[1] == 11 && p[2] == 22 && p[3] == 33);
}

static void test_kheap_free_reuse(void)
{
    pmm_init();
    kheap_init();
    void *a = kmalloc(64);
    kfree(a);
    void *b = kmalloc(64);
    KASSERT(b == a);     // a same-size request reuses the freed block
}

static void test_kheap_coalesce(void)
{
    pmm_init();
    kheap_init();
    char *a = kmalloc(32);
    char *b = kmalloc(64);
    char *c = kmalloc(16);
    (void)c;
    kfree(b);
    kfree(a);                  // merges a + b into one chunk
    char *big = kmalloc(80);   // too big for a(32) or b(64) alone
    KASSERT(big == a);         // only fits because the two coalesced
}

// --- Threads / scheduler ---

// A no-op thread body used only to take its address in the test below.
static void noop_thread(void *arg) { (void)arg; }

static void test_thread_create_context(void)
{
    pmm_init();
    kheap_init();
    void *arg = (void *)(uintptr_t)0xABCD;
    struct thread *t = thread_create(noop_thread, arg, 1);

    KASSERT(t != 0);
    KASSERT(t->stack != 0);
    KASSERT(t->state == THREAD_RUNNABLE);
    // The initial stack pointer must sit inside the allocated stack.
    KASSERT(t->ctx.sp > (uint64_t)(uintptr_t)t->stack);
    // First switch must land in the trampoline, with fn/arg staged in x19/x20.
    KASSERT(t->ctx.lr  == (uint64_t)(uintptr_t)thread_trampoline);
    KASSERT(t->ctx.x19 == (uint64_t)(uintptr_t)noop_thread);
    KASSERT(t->ctx.x20 == (uint64_t)(uintptr_t)arg);
}

// Shared log the round-robin workers append to, proving switch ordering.
static int rr_order[16];
static int rr_n;

static void rr_worker(void *arg)
{
    int id = (int)(uintptr_t)arg;
    for (int k = 0; k < 3; k++) {
        rr_order[rr_n++] = id;   // record that this thread ran
        yield();                 // cooperatively hand off to the next thread
    }
    // returning here -> thread_trampoline calls thread_exit
}

static void test_round_robin_order(void)
{
    pmm_init();
    kheap_init();
    rr_n = 0;

    sched_init();                            // boot thread becomes thread 0
    thread_create(rr_worker, (void *)(uintptr_t)1, 1);
    thread_create(rr_worker, (void *)(uintptr_t)2, 1);

    // The boot thread yields until the two workers have logged 6 entries.
    while (rr_n < 6) {
        yield();
    }

    // Deterministic round-robin: worker 1, worker 2, repeating.
    KASSERT(rr_order[0] == 1);
    KASSERT(rr_order[1] == 2);
    KASSERT(rr_order[2] == 1);
    KASSERT(rr_order[3] == 2);
    KASSERT(rr_order[4] == 1);
    KASSERT(rr_order[5] == 2);
}

static void test_time_slice_expiry(void)
{
    sched_init();   // resets the slice counter to a full SCHED_TIME_SLICE

    // The first SCHED_TIME_SLICE-1 ticks do NOT expire the slice.
    for (int i = 0; i < SCHED_TIME_SLICE - 1; i++) {
        KASSERT(sched_tick() == 0);
    }
    // The SCHED_TIME_SLICE-th tick expires it -> reschedule signal.
    KASSERT(sched_tick() == 1);

    // ...and it resets: the same pattern repeats.
    for (int i = 0; i < SCHED_TIME_SLICE - 1; i++) {
        KASSERT(sched_tick() == 0);
    }
    KASSERT(sched_tick() == 1);
}

static int pri_log[8];
static int pri_n;
static void pri_hi(void *a) { (void)a; pri_log[pri_n++] = 2; }  // returns -> exits
static void pri_lo(void *a) { (void)a; pri_log[pri_n++] = 1; }

static void test_priority_order(void)
{
    pmm_init();
    kheap_init();
    pri_n = 0;

    sched_init();
    thread_create(pri_lo, 0, 1);   // LOW priority, created FIRST
    thread_create(pri_hi, 0, 2);   // HIGH priority, created SECOND

    while (pri_n < 2) {            // idle thread yields until both have run
        yield();
    }

    // Strict priority: the high-priority thread runs first, despite being
    // created second (round-robin alone would give 1,2).
    KASSERT(pri_log[0] == 2);
    KASSERT(pri_log[1] == 1);
}

static char slp_log[8];
static int slp_n;
static void slp_worker(void *a)
{
    (void)a;
    slp_log[slp_n++] = 'S';
    sleep_ticks(3);
    slp_log[slp_n++] = 'W';
}

static void test_sleep_wakes_after_ticks(void)
{
    pmm_init();
    kheap_init();
    slp_n = 0;

    sched_init();                          // jiffies := 0
    thread_create(slp_worker, 0, 1);       // priority above idle

    yield();                               // run worker: logs 'S', sleeps 3 ticks
    KASSERT(slp_n == 1);
    KASSERT(slp_log[0] == 'S');

    sched_tick(); yield(); KASSERT(slp_n == 1);   // tick 1: still asleep
    sched_tick(); yield(); KASSERT(slp_n == 1);   // tick 2: still asleep
    sched_tick(); yield();                        // tick 3: wakes
    KASSERT(slp_n == 2);
    KASSERT(slp_log[1] == 'W');
}

// --- sched_block / sched_wake: the V6 sleep/wakeup primitive (Phase 1) ---
static int blk_chan;                       // an arbitrary wait-channel address
static char blk_log[8];
static int blk_n;
static void blk_worker(void *a)
{
    (void)a;
    blk_log[blk_n++] = 'B';
    sched_block(&blk_chan);                // sleep until sched_wake(&blk_chan)
    blk_log[blk_n++] = 'W';
}

static void test_block_wakes_on_channel(void)
{
    pmm_init();
    kheap_init();
    blk_n = 0;

    sched_init();
    thread_create(blk_worker, 0, 1);       // priority above idle

    yield();                               // worker runs, logs 'B', blocks
    KASSERT(blk_n == 1);
    KASSERT(blk_log[0] == 'B');

    // The timer must NOT wake a channel-blocked thread (unlike a timed sleep).
    sched_tick(); yield(); KASSERT(blk_n == 1);
    sched_tick(); yield(); KASSERT(blk_n == 1);

    // A wake on a DIFFERENT channel leaves it blocked.
    int other = 0;
    sched_wake(&other); yield(); KASSERT(blk_n == 1);

    // The matching wake makes it runnable; it then finishes.
    sched_wake(&blk_chan);
    yield();
    KASSERT(blk_n == 2);
    KASSERT(blk_log[1] == 'W');
}

// A pending signal also wakes a blocked thread (the EINTR path).
static int blk_sig_chan;
static int blk_sig_n;
static struct thread *blk_sig_thread;
static void blk_sig_worker(void *a)
{
    (void)a;
    sched_block(&blk_sig_chan);            // never explicitly woken -- only by the signal
    blk_sig_n++;
}
static void test_block_woken_by_signal(void)
{
    pmm_init();
    kheap_init();
    blk_sig_n = 0;

    sched_init();
    blk_sig_thread = thread_create(blk_sig_worker, 0, 1);

    yield();                               // worker blocks
    KASSERT(blk_sig_n == 0);

    signal_send(blk_sig_thread, SIGTERM);  // posting a signal must unblock it
    yield();
    KASSERT(blk_sig_n == 1);               // it ran past sched_block()
}

// sched_wait_event: a channel wait with a timeout (Phase 3 foundation).
static int we_chan;
static int we_n;
static void we_worker(void *a) { (void)a; sched_wait_event(&we_chan, 3); we_n++; }
static void test_wait_event_times_out(void)
{
    pmm_init(); kheap_init(); we_n = 0;
    sched_init();
    thread_create(we_worker, 0, 1);
    yield();                               // worker sleeps on the channel (deadline +3)
    KASSERT(we_n == 0);
    sched_tick(); yield(); KASSERT(we_n == 0);   // tick 1
    sched_tick(); yield(); KASSERT(we_n == 0);   // tick 2
    sched_tick(); yield();                        // tick 3: deadline -> wakes
    KASSERT(we_n == 1);
}
static int we2_chan;
static int we2_n;
static void we2_worker(void *a) { (void)a; sched_wait_event(&we2_chan, 1000); we2_n++; }
static void test_wait_event_early_wake(void)
{
    pmm_init(); kheap_init(); we2_n = 0;
    sched_init();
    thread_create(we2_worker, 0, 1);
    yield();                               // worker sleeps (long deadline)
    KASSERT(we2_n == 0);
    sched_wake(&we2_chan);                  // a packet "arrives" -> wake early
    yield();
    KASSERT(we2_n == 1);                    // woke well before the 1000-tick deadline
}

// --- console line discipline (Phase 2) ---
static void noop_worker(void *a) { (void)a; }

static void test_console_ring_fifo(void)
{
    // Bytes fed to the line discipline come back from console_getc() in order.
    // (Non-empty ring, so console_getc returns immediately without blocking.)
    console_input('h');
    console_input('i');
    KASSERT(console_getc() == 'h');
    KASSERT(console_getc() == 'i');
}

static void test_console_ctrlc_signals_foreground(void)
{
    pmm_init(); kheap_init(); sched_init();
    struct thread *t = thread_create(noop_worker, 0, 1);
    sched_set_foreground(t);

    console_input(3);                      // Ctrl-C
    KASSERT(t->sig_pending & (1ull << SIGINT));   // -> SIGINT to the foreground
    KASSERT(!console_has_input());         // and the 0x03 was NOT queued

    sched_set_foreground(0);
}

// --- System calls (do_syscall dispatch) ---

static void test_syscall_write_returns_len(void)
{
    struct trapframe tf;
    const char *s = "hello";
    tf.x[8] = SYS_WRITE;
    tf.x[0] = 1;                            // fd = stdout
    tf.x[1] = (uint64_t)(uintptr_t)s;
    tf.x[2] = 5;
    do_syscall(&tf);
    KASSERT(tf.x[0] == 5);   // returns bytes written
}

static void test_syscall_unknown(void)
{
    struct trapframe tf;
    tf.x[8] = 999;
    do_syscall(&tf);
    KASSERT(tf.x[0] == (uint64_t)-1);
}

static void test_syscall_yield(void)
{
    pmm_init(); kheap_init();
    sched_init();
    struct trapframe tf;
    tf.x[8] = SYS_YIELD;
    do_syscall(&tf);
    KASSERT(tf.x[0] == 0);
}

static void test_syscall_return_in_x0(void)
{
    pmm_init(); kheap_init();
    sched_init();
    struct trapframe tf;
    tf.x[8] = SYS_GETPID;
    tf.x[0] = 0xDEAD;          // do_syscall must overwrite this with the result
    do_syscall(&tf);
    KASSERT(tf.x[0] == 0);     // GETPID from the idle thread (id 0)
}

static long sc_pid;
static void sc_getpid_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    tf.x[8] = SYS_GETPID;
    do_syscall(&tf);
    sc_pid = (long)tf.x[0];
}
static void test_syscall_getpid(void)
{
    pmm_init(); kheap_init();
    sc_pid = -999;
    sched_init();
    thread_create(sc_getpid_worker, 0, 1);   // first created -> id 1
    while (sc_pid == -999) {
        yield();
    }
    KASSERT(sc_pid == 1);                     // getpid returned the worker's id
}

static char sc_slp_log[8];
static int sc_slp_n;
static void sc_sleep_worker(void *a)
{
    (void)a;
    sc_slp_log[sc_slp_n++] = 'S';
    struct trapframe tf;
    tf.x[8] = SYS_SLEEP;
    tf.x[0] = 3;
    do_syscall(&tf);
    sc_slp_log[sc_slp_n++] = 'W';
}
static void test_syscall_sleep_blocks(void)
{
    pmm_init(); kheap_init();
    sc_slp_n = 0;
    sched_init();
    thread_create(sc_sleep_worker, 0, 1);
    yield(); KASSERT(sc_slp_n == 1); KASSERT(sc_slp_log[0] == 'S');
    sched_tick(); yield(); KASSERT(sc_slp_n == 1);
    sched_tick(); yield(); KASSERT(sc_slp_n == 1);
    sched_tick(); yield();
    KASSERT(sc_slp_n == 2); KASSERT(sc_slp_log[1] == 'W');
}

static char sc_exit_log[4];
static int sc_exit_n;
static void sc_exit_worker(void *a)
{
    (void)a;
    sc_exit_log[sc_exit_n++] = 'B';
    struct trapframe tf;
    tf.x[8] = SYS_EXIT;
    do_syscall(&tf);                       // never returns
    sc_exit_log[sc_exit_n++] = 'A';        // unreachable
}
static void test_syscall_exit_ends_thread(void)
{
    pmm_init(); kheap_init();
    sc_exit_n = 0;
    sched_init();
    thread_create(sc_exit_worker, 0, 1);
    while (sc_exit_n < 1) {
        yield();
    }
    yield(); yield();                      // give 'A' a chance to (wrongly) run
    KASSERT(sc_exit_n == 1);
    KASSERT(sc_exit_log[0] == 'B');
}

// --- Per-process address spaces (isolation at the page-table level) ---

static void test_as_data_is_private(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *a = as_create();
    struct addrspace *b = as_create();
    uint64_t pa_a = as_translate(a, USER_DATA_VA);
    uint64_t pa_b = as_translate(b, USER_DATA_VA);
    KASSERT(pa_a != 0 && pa_b != 0);
    KASSERT(pa_a != pa_b);                 // private: different physical pages
}

static void test_as_image_maps_code(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *a = as_create();
    uint64_t pa = as_translate(a, USER_CODE_VA);
    KASSERT(pa != 0);
    // The first byte at USER_CODE_VA equals the embedded program's start
    // (as_create() flat-loads sh_elf; for this test only the mapping matters).
    extern unsigned char sh_elf[];
    KASSERT(*(volatile uint8_t *)(uintptr_t)pa == sh_elf[0]);
}

static void test_as_kernel_shared(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *a = as_create();
    KASSERT(as_translate(a, 0x40080000UL) == 0x40080000UL);   // identity kernel map
}

static void test_as_unmapped_returns_zero(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *a = as_create();
    KASSERT(as_translate(a, 0x10000000000UL) == 0);           // 1 TiB: nothing there
}

static void test_as_stack_is_private(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *a = as_create();
    struct addrspace *b = as_create();
    uint64_t va = USER_STACK_TOP - 16;     // an address on the user stack
    KASSERT(as_translate(a, va) != 0);
    KASSERT(as_translate(a, va) != as_translate(b, va));
}

// --- VFS / ramfs ---

static int bytes_eq(const void *a, const void *b, uint64_t n)
{
    const uint8_t *x = a, *y = b;
    for (uint64_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return 0;
    }
    return 1;
}

static void test_vfs_mount_root_is_dir(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    struct vnode *r = vfs_root();
    KASSERT(r != 0);
    KASSERT(r->type == VN_DIR);
}

static void test_vfs_create_and_lookup(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    KASSERT(vfs_create("/a.txt", VN_FILE) != 0);
    struct vnode *vn = vfs_lookup("/a.txt");
    KASSERT(vn != 0);
    KASSERT(vn->type == VN_FILE);
}

static void test_vfs_write_then_read(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    vfs_create("/a.txt", VN_FILE);
    struct file *w = vfs_open("/a.txt");
    KASSERT(vfs_write(w, "hello", 5) == 5);
    vfs_close(w);
    struct file *r = vfs_open("/a.txt");
    char buf[8] = {0};
    KASSERT(vfs_read(r, buf, 5) == 5);
    KASSERT(bytes_eq(buf, "hello", 5));
    vfs_close(r);
}

static void test_vfs_read_offset(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    vfs_create("/a.txt", VN_FILE);
    struct file *w = vfs_open("/a.txt");
    vfs_write(w, "abcdef", 6);
    vfs_close(w);
    struct file *r = vfs_open("/a.txt");
    r->off = 2;                          // seek to 'c'
    char buf[4] = {0};
    KASSERT(vfs_read(r, buf, 3) == 3);
    KASSERT(bytes_eq(buf, "cde", 3));
    vfs_close(r);
}

static void test_vfs_write_grows(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    vfs_create("/a.txt", VN_FILE);
    struct file *w = vfs_open("/a.txt");
    vfs_write(w, "aaaaa", 5);
    vfs_write(w, "bbbbbbbbbb", 10);      // total 15
    vfs_close(w);
    struct vnode *vn = vfs_lookup("/a.txt");
    KASSERT(vn->size == 15);
    struct file *r = vfs_open("/a.txt");
    char buf[16] = {0};
    KASSERT(vfs_read(r, buf, 15) == 15);
    KASSERT(bytes_eq(buf, "aaaaabbbbbbbbbb", 15));
    vfs_close(r);
}

static void test_vfs_readdir_lists(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    vfs_create("/one", VN_FILE);
    vfs_create("/two", VN_FILE);
    vfs_create("/three", VN_FILE);
    struct vnode *root = vfs_root();
    int count = 0;
    char name[32];
    int found_two = 0;
    while (vfs_readdir(root, count, name) == 0) {
        if (name[0] == 't' && name[1] == 'w' && name[2] == 'o' && name[3] == 0) {
            found_two = 1;
        }
        count++;
    }
    KASSERT(count == 3);
    KASSERT(found_two);
}

static void test_vfs_lookup_missing(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    KASSERT(vfs_lookup("/nope") == 0);
}

static void test_vfs_nested_dir(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    KASSERT(vfs_create("/d", VN_DIR) != 0);
    KASSERT(vfs_create("/d/f.txt", VN_FILE) != 0);
    KASSERT(vfs_lookup("/d/f.txt") != 0);
    KASSERT(vfs_lookup("/d")->type == VN_DIR);
}

static void test_initrd_unpacked(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    initrd_unpack();
    struct file *f = vfs_open("/hello.txt");
    KASSERT(f != 0);
    char buf[16] = {0};
    int n = vfs_read(f, buf, 14);
    KASSERT(n == 14);
    KASSERT(bytes_eq(buf, "Hello, MyOSv2!\n", 14));
    vfs_close(f);
}

// --- File descriptors (open/read/close via syscalls, in a worker thread) ---

static long fd_res;
static void fd_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    tf.x[8] = SYS_OPEN; tf.x[0] = (uint64_t)(uintptr_t)"/hello.txt";
    do_syscall(&tf);
    fd_res = (long)tf.x[0];
}
static void test_fd_open_returns_fd(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type()); initrd_unpack();
    fd_res = -1;
    sched_init();
    thread_create(fd_worker, 0, 1);
    while (fd_res == -1) { yield(); }
    KASSERT(fd_res >= 3);
}

static char fd_buf[8];
static long fd_n;
static void fd_read_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    tf.x[8] = SYS_OPEN; tf.x[0] = (uint64_t)(uintptr_t)"/hello.txt";
    do_syscall(&tf);
    long fd = (long)tf.x[0];
    tf.x[8] = SYS_READ; tf.x[0] = (uint64_t)fd;
    tf.x[1] = (uint64_t)(uintptr_t)fd_buf; tf.x[2] = 5;
    do_syscall(&tf);
    fd_n = (long)tf.x[0];
}
static void test_fd_read_syscall(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type()); initrd_unpack();
    fd_n = -1;
    sched_init();
    thread_create(fd_read_worker, 0, 1);
    while (fd_n == -1) { yield(); }
    KASSERT(fd_n == 5);
    KASSERT(bytes_eq(fd_buf, "Hello", 5));
}

static long fd_miss;
static void fd_miss_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    tf.x[8] = SYS_OPEN; tf.x[0] = (uint64_t)(uintptr_t)"/nope";
    do_syscall(&tf);
    fd_miss = (long)tf.x[0];
}
static void test_fd_open_missing(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type()); initrd_unpack();
    fd_miss = 0;
    sched_init();
    thread_create(fd_miss_worker, 0, 1);
    while (fd_miss == 0) { yield(); }
    KASSERT(fd_miss == -1);
}

static long fd_a, fd_b;
static void fd_reuse_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    tf.x[8] = SYS_OPEN; tf.x[0] = (uint64_t)(uintptr_t)"/hello.txt";
    do_syscall(&tf); fd_a = (long)tf.x[0];
    tf.x[8] = SYS_CLOSE; tf.x[0] = (uint64_t)fd_a; do_syscall(&tf);
    tf.x[8] = SYS_OPEN; tf.x[0] = (uint64_t)(uintptr_t)"/hello.txt";
    do_syscall(&tf); fd_b = (long)tf.x[0];
}
static void test_fd_close_reuse(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type()); initrd_unpack();
    fd_a = fd_b = -1;
    sched_init();
    thread_create(fd_reuse_worker, 0, 1);
    while (fd_b == -1) { yield(); }
    KASSERT(fd_a == fd_b);
}

// --- Copy-on-write (page-level) ---

static void test_cow_clone_shares_pages(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *p = as_create();
    struct addrspace *c = as_clone(p);
    KASSERT(as_translate(p, USER_DATA_VA) == as_translate(c, USER_DATA_VA));
    KASSERT(as_translate(c, USER_DATA_VA) != 0);
}
static void test_cow_clone_refcount(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *p = as_create();
    struct addrspace *c = as_clone(p);
    (void)c;
    uint64_t pa = as_translate(p, USER_DATA_VA);
    KASSERT(page_refcount(pa) == 2);
}
static void test_cow_fault_copies(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *p = as_create();
    uint64_t ppa = as_translate(p, USER_DATA_VA);
    *(volatile uint8_t *)(uintptr_t)ppa = 0x5A;          // marker (parent page is RW)
    struct addrspace *c = as_clone(p);
    KASSERT(cow_fault(c, USER_DATA_VA) == 1);
    uint64_t cpa = as_translate(c, USER_DATA_VA);
    KASSERT(cpa != ppa);                                  // private now
    KASSERT(*(volatile uint8_t *)(uintptr_t)cpa == 0x5A); // content preserved
}
static void test_cow_fault_refcount(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *p = as_create();
    uint64_t pa = as_translate(p, USER_DATA_VA);
    struct addrspace *c = as_clone(p);
    KASSERT(page_refcount(pa) == 2);
    cow_fault(c, USER_DATA_VA);
    KASSERT(page_refcount(pa) == 1);
}
static void test_cow_fault_non_cow(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *p = as_create();
    KASSERT(cow_fault(p, 0x999000UL) == 0);
}

static char rd_name[32];
static long rd_r0, rd_r1;
static void rd_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    tf.x[8] = SYS_READDIR; tf.x[0] = (uint64_t)(uintptr_t)"/"; tf.x[1] = 0;
    tf.x[2] = (uint64_t)(uintptr_t)rd_name;
    do_syscall(&tf); rd_r0 = (long)tf.x[0];
    tf.x[8] = SYS_READDIR; tf.x[0] = (uint64_t)(uintptr_t)"/"; tf.x[1] = 999;
    tf.x[2] = (uint64_t)(uintptr_t)rd_name;
    do_syscall(&tf); rd_r1 = (long)tf.x[0];
}
static void test_syscall_readdir(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type()); initrd_unpack();
    rd_r0 = rd_r1 = -2;
    sched_init();
    thread_create(rd_worker, 0, 1);
    while (rd_r0 == -2 || rd_r1 == -2) { yield(); }
    KASSERT(rd_r0 == 0);     // first entry exists
    KASSERT(rd_r1 == -1);    // past the end
}

// --- ASIDs (Phase 11) ---

static void test_asid_assigned_nonzero(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *a = as_create();
    KASSERT(a->asid != 0);             // 0 is reserved (boot/unused TTBR0)
}

static void test_asid_unique(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *a = as_create();
    struct addrspace *b = as_create();
    KASSERT(a->asid != b->asid);       // distinct address spaces, distinct IDs
}

static void test_asid_clone_distinct(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *p = as_create();
    struct addrspace *c = as_clone(p);
    KASSERT(c->asid != p->asid);       // a clone is its own address space
}

static void test_asid_user_page_nonglobal(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *a = as_create();
    uint64_t *pte = as_pte(a, USER_DATA_VA);
    KASSERT(pte != 0);
    KASSERT((*pte & (1UL << 11)) != 0);   // nG set -> ASID-tagged
}

static void test_asid_rollover_recycles(void)
{
    vm_init();                            // next_asid = 1
    uint16_t first = asid_alloc();        // 1
    for (uint32_t i = 1; i < 0xFFFF; i++) { asid_alloc(); }   // consume up to ASID_MAX
    uint16_t wrapped = asid_alloc();      // ASID_MAX+1 -> wrap
    KASSERT(first == 1);
    KASSERT(wrapped == 1);                // recycled from the bottom
}

// --- Process lifecycle: exec + exit + wait (Phase 13) ---

static void test_asid_free_recycles(void)
{
    vm_init();
    uint16_t a = asid_alloc();
    uint16_t b = asid_alloc();
    asid_free(a);
    KASSERT(asid_alloc() == a);   // a freed ASID is handed back before a fresh one
    (void)b;
}

static void test_as_destroy_frees_pages(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *as = as_create();
    uint64_t pa = as_translate(as, USER_DATA_VA);
    KASSERT(page_refcount(pa) == 1);
    as_destroy(as);
    KASSERT(page_refcount(pa) == 0);   // the page was returned to the PMM
}

static void test_as_destroy_recycles_asid(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *as = as_create();
    uint16_t a = as->asid;
    as_destroy(as);
    KASSERT(asid_alloc() == a);         // its ASID is reusable
}

static void wexit7(void *a) { (void)a; thread_exit(7); }

static void test_wait_reaps_child(void)
{
    pmm_init(); kheap_init(); vm_init();
    sched_init();
    thread_create(wexit7, 0, 1);
    int st = -1;
    int pid = sched_wait(&st);
    KASSERT(pid > 0);                    // reaped a child
    KASSERT(st == 7);                    // with its exit status
    KASSERT(sched_wait(&st) == -1);      // and it's gone (no children remain)
}

static void test_wait_no_children(void)
{
    pmm_init(); kheap_init(); vm_init();
    sched_init();
    int st = 0;
    KASSERT(sched_wait(&st) == -1);      // nothing to wait for
}

static void test_exec_missing_returns_neg1(void)
{
    pmm_init(); kheap_init(); vm_init();
    vfs_mount_root(ramfs_type());        // empty root: no files to exec
    struct trapframe tf;
    KASSERT(proc_exec(&tf, "/no/such/file", 0) == -1);   // clean failure, no swap
}

// Read a little-endian 64-bit word at user VA `v` from the top stack page,
// reached through the identity-mapped physical window `page` (base VA `pbase`).
static uint64_t stack_u64(const uint8_t *page, uint64_t pbase, uint64_t v)
{
    uint64_t x = 0;
    for (int b = 0; b < 8; b++) { x |= (uint64_t)page[(v - pbase) + b] << (b * 8); }
    return x;
}

static int build_min_elf(uint8_t *buf, uint64_t vaddr, const uint8_t *code, int clen);

static void test_exec_argv_on_stack(void)
{
    pmm_init(); kheap_init(); vm_init();
    uint8_t img[256];
    uint8_t code[4] = {0, 0, 0, 0};
    int n = build_min_elf(img, USER_CODE_VA, code, 4);
    uint64_t entry = 0;
    struct addrspace *as = as_create_elf(img, (uint64_t)n, &entry);
    KASSERT(as != 0);

    char *argv[] = { "ping", "example.com", 0 };
    int argc = -1;
    uint64_t sp = proc_setup_argv(as, argv, &argc);
    KASSERT(argc == 2);
    KASSERT((sp & 15) == 0);                       // AArch64 needs a 16-aligned sp

    uint64_t pbase = USER_STACK_TOP - 0x1000;      // VA of the top stack page
    uint8_t *page = (uint8_t *)(uintptr_t)as_translate(as, pbase);
    uint64_t a0 = stack_u64(page, pbase, sp);      // argv[0]
    uint64_t a1 = stack_u64(page, pbase, sp + 8);  // argv[1]
    uint64_t aN = stack_u64(page, pbase, sp + 16); // argv[2]
    KASSERT(aN == 0);                              // NULL-terminated

    const char *s0 = (const char *)&page[a0 - pbase];
    const char *s1 = (const char *)&page[a1 - pbase];
    KASSERT(s0[0]=='p' && s0[1]=='i' && s0[2]=='n' && s0[3]=='g' && s0[4]==0);
    KASSERT(s1[0]=='e' && s1[10]=='m' && s1[11]==0);   // "example.com" (len 11)
}

// --- ELF loader (Phase 14) ---

static void test_as_map_segment_bss_zeroed(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *as = (struct addrspace *)pmm_alloc();
    as->l0 = as_alloc_l0();
    as->asid = asid_alloc();
    uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    as_map_segment(as, USER_CODE_VA, data, 4, 16, /*writable*/1, /*exec*/0);
    uint64_t pa = as_translate(as, USER_CODE_VA);
    uint8_t *p = (uint8_t *)(uintptr_t)pa;
    KASSERT(p[0] == 0xAA && p[3] == 0xDD);   // file part copied
    KASSERT(p[4] == 0 && p[15] == 0);        // bss tail (beyond filesz) zeroed
}

// Construct a minimal valid ELF64/AArch64 image: header + one PT_LOAD segment of
// `clen` bytes at `vaddr`, with the bytes at file offset 128. Returns the total
// image length.
static int build_min_elf(uint8_t *buf, uint64_t vaddr, const uint8_t *code, int clen)
{
    for (int i = 0; i < 256; i++) { buf[i] = 0; }
    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
    eh->e_ident[0] = 0x7f; eh->e_ident[1] = 'E'; eh->e_ident[2] = 'L'; eh->e_ident[3] = 'F';
    eh->e_ident[4] = 2;            // ELFCLASS64
    eh->e_ident[5] = 1;            // ELFDATA2LSB
    eh->e_type = 2;                // ET_EXEC
    eh->e_machine = 0xB7;          // EM_AARCH64
    eh->e_entry = vaddr;
    eh->e_phoff = 64;
    eh->e_ehsize = 64;
    eh->e_phentsize = 56;
    eh->e_phnum = 1;
    Elf64_Phdr *ph = (Elf64_Phdr *)(buf + 64);
    ph->p_type = PT_LOAD;
    ph->p_flags = PF_R | PF_X;
    ph->p_offset = 128;
    ph->p_vaddr = vaddr;
    ph->p_filesz = (uint64_t)clen;
    ph->p_memsz = (uint64_t)clen;
    for (int i = 0; i < clen; i++) { buf[128 + i] = code[i]; }
    return 128 + clen;
}

static void test_elf_rejects_bad_magic(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *as = (struct addrspace *)pmm_alloc();
    as->l0 = as_alloc_l0(); as->asid = asid_alloc();
    uint8_t junk[64];
    for (int i = 0; i < 64; i++) { junk[i] = 0; }
    uint64_t entry = 0;
    KASSERT(elf_load(as, junk, sizeof(junk), &entry) == -1);
}

static void test_elf_entry_and_segment(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *as = (struct addrspace *)pmm_alloc();
    as->l0 = as_alloc_l0(); as->asid = asid_alloc();
    uint8_t code[4] = {0x11, 0x22, 0x33, 0x44};
    static uint8_t img[256];
    int len = build_min_elf(img, USER_CODE_VA, code, 4);
    uint64_t entry = 0;
    KASSERT(elf_load(as, img, (uint64_t)len, &entry) == 0);
    KASSERT(entry == USER_CODE_VA);
    uint64_t pa = as_translate(as, USER_CODE_VA);
    uint8_t *p = (uint8_t *)(uintptr_t)pa;
    KASSERT(p[0] == 0x11 && p[3] == 0x44);
}

static void test_as_create_elf_has_stack(void)
{
    pmm_init(); kheap_init(); vm_init();
    extern unsigned char sh_elf[]; extern unsigned int sh_elf_len;
    uint64_t entry = 0;
    struct addrspace *as = as_create_elf(sh_elf, sh_elf_len, &entry);
    KASSERT(as != 0);
    KASSERT(entry == USER_CODE_VA);                         // sh's _start
    KASSERT(as_translate(as, USER_STACK_TOP - 0x1000) != 0); // a stack page exists
    KASSERT(as_translate(as, USER_CODE_VA) != 0);          // code segment mapped
}

// --- User heap: sbrk (Phase 15) ---

static struct addrspace *fresh_elf_as(void)
{
    extern unsigned char sh_elf[]; extern unsigned int sh_elf_len;
    uint64_t entry = 0;
    return as_create_elf(sh_elf, sh_elf_len, &entry);
}

static void test_sbrk_grows_and_maps(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *as = fresh_elf_as();
    KASSERT(as_sbrk(as, 0) == USER_HEAP_BASE);              // initial break
    KASSERT(as_sbrk(as, 100) == USER_HEAP_BASE);            // returns the OLD break
    uint64_t pa = as_translate(as, USER_HEAP_BASE);
    KASSERT(pa != 0);                                       // a page got mapped
    KASSERT(*(volatile uint8_t *)(uintptr_t)pa == 0);       // demand-zeroed
    KASSERT(as_sbrk(as, 0) == USER_HEAP_BASE + 100);        // break advanced
}

static void test_sbrk_multi_page(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *as = fresh_elf_as();
    as_sbrk(as, 8192 + 1);                                  // spans 3 pages
    KASSERT(as_translate(as, USER_HEAP_BASE) != 0);
    KASSERT(as_translate(as, USER_HEAP_BASE + 4096) != 0);
    KASSERT(as_translate(as, USER_HEAP_BASE + 8192) != 0);
}

// --- mmap (Phase 16) ---

static void test_mmap_maps_zeroed(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *as = fresh_elf_as();
    uint64_t va = as_mmap(as, 100);
    KASSERT(va >= USER_MMAP_BASE);
    uint64_t pa = as_translate(as, va);
    KASSERT(pa != 0);
    KASSERT(*(volatile uint8_t *)(uintptr_t)pa == 0);       // demand-zeroed
}

static void test_mmap_two_distinct(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *as = fresh_elf_as();
    uint64_t a = as_mmap(as, 4096), b = as_mmap(as, 4096);
    KASSERT(a != b);
    KASSERT(as_translate(as, a) != as_translate(as, b));    // distinct pages
}

static void test_munmap_unmaps(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *as = fresh_elf_as();
    uint64_t va = as_mmap(as, 4096);
    KASSERT(as_translate(as, va) != 0);
    as_munmap(as, va, 4096);
    KASSERT(as_translate(as, va) == 0);
}

// --- shared memory (Phase 16) ---

static void test_shm_create_handle(void)
{
    pmm_init(); kheap_init(); vm_init(); shm_init();
    KASSERT(shm_create(4096) >= 0);
}

static void test_shm_shared_pages(void)
{
    pmm_init(); kheap_init(); vm_init(); shm_init();
    struct addrspace *as1 = fresh_elf_as();
    struct addrspace *as2 = fresh_elf_as();
    int h = shm_create(4096);
    KASSERT(h >= 0);
    uint64_t v1 = shm_map(as1, h), v2 = shm_map(as2, h);
    KASSERT(v1 && v2);
    uint64_t p1 = as_translate(as1, v1), p2 = as_translate(as2, v2);
    KASSERT(p1 == p2);                                   // SAME physical page
    *(volatile uint8_t *)(uintptr_t)p1 = 0x5A;           // write via as1
    KASSERT(*(volatile uint8_t *)(uintptr_t)p2 == 0x5A); // visible via as2
}

static void test_shm_survives_unmap(void)
{
    pmm_init(); kheap_init(); vm_init(); shm_init();
    struct addrspace *as = fresh_elf_as();
    int h = shm_create(4096);
    uint64_t v = shm_map(as, h);
    uint64_t pa = as_translate(as, v);
    KASSERT(page_refcount(pa) == 2);                     // table ref + this mapping
    as_destroy(as);
    KASSERT(page_refcount(pa) == 1);                     // table keeps it alive
}

// --- pipes (Phase 17) ---

static void test_pipe_write_then_read(void)
{
    pmm_init(); kheap_init();
    struct pipe *p = pipe_alloc();
    struct file wf = { .pipe = p, .writable = 1, .ref = 1 };
    struct file rf = { .pipe = p, .writable = 0, .ref = 1 };
    KASSERT(pipe_write(&wf, "hello", 5) == 5);
    char b[8] = {0};
    KASSERT(pipe_read(&rf, b, 8) == 5);
    KASSERT(b[0] == 'h' && b[4] == 'o');
    KASSERT(p->count == 0);
}

static void test_pipe_eof(void)
{
    pmm_init(); kheap_init();
    struct pipe *p = pipe_alloc();
    p->writers = 0;                          // no writers, buffer empty
    struct file rf = { .pipe = p, .writable = 0, .ref = 1 };
    char b[4];
    KASSERT(pipe_read(&rf, b, 4) == 0);      // EOF
}

static void test_pipe_broken(void)
{
    pmm_init(); kheap_init();
    struct pipe *p = pipe_alloc();
    p->readers = 0;                          // no readers
    p->count = PIPE_SIZE; p->w = 0; p->r = 0;  // full
    struct file wf = { .pipe = p, .writable = 1, .ref = 1 };
    char big[8]; for (int i = 0; i < 8; i++) { big[i] = 'x'; }
    KASSERT(pipe_write(&wf, big, 8) == -1);  // broken pipe
}

static void test_pipe_wraps(void)
{
    pmm_init(); kheap_init();
    struct pipe *p = pipe_alloc();
    struct file wf = { .pipe = p, .writable = 1, .ref = 1 };
    struct file rf = { .pipe = p, .writable = 0, .ref = 1 };
    p->r = p->w = PIPE_SIZE - 10;            // force a wrap mid-transfer
    char buf[50]; for (int i = 0; i < 50; i++) { buf[i] = (char)i; }
    KASSERT(pipe_write(&wf, buf, 50) == 50);
    char out[50]; for (int i = 0; i < 50; i++) { out[i] = 0; }
    KASSERT(pipe_read(&rf, out, 50) == 50);
    for (int i = 0; i < 50; i++) { KASSERT(out[i] == (char)i); }
}

static void test_file_refcount(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    vfs_create("/f", VN_FILE);
    struct file *f = vfs_open("/f");
    KASSERT(f->ref == 1);
    file_dup(f);
    KASSERT(f->ref == 2);
    vfs_close(f);                            // ref 2 -> 1, NOT freed
    KASSERT(f->ref == 1);
    KASSERT(vfs_write(f, "x", 1) == 1);      // still usable
    vfs_close(f);                            // ref 1 -> 0, freed (no crash after)
}

// --- signals (Phase 18) ---

static void sig_noop(void *a) { (void)a; for (;;) { yield(); } }

static void test_kill_sets_pending(void)
{
    pmm_init(); kheap_init(); vm_init(); sched_init();
    struct thread *t = thread_create(sig_noop, 0, 1);
    signal_send(t, SIGINT);
    KASSERT(t->sig_pending & (1ull << SIGINT));
}

static void test_kill_by_pid(void)
{
    pmm_init(); kheap_init(); vm_init(); sched_init();
    struct thread *t = thread_create(sig_noop, 0, 1);
    KASSERT(sched_kill(t->id, SIGTERM) == 0);
    KASSERT(t->sig_pending & (1ull << SIGTERM));
    KASSERT(sched_kill(9999, SIGTERM) == -1);   // no such pid
}

static void test_sig_default_vs_handler(void)
{
    pmm_init(); kheap_init(); vm_init(); sched_init();
    struct thread *t = thread_create(sig_noop, 0, 1);
    KASSERT(signal_action(t, SIGTERM) == 0);            // 0 = default (terminate)
    t->sig_handler[SIGINT] = (uint64_t (*)(int))0x8000000040ULL;
    KASSERT(signal_action(t, SIGINT) == 0x8000000040ULL);
    t->sig_handler[SIGKILL] = (uint64_t (*)(int))0x8000000040ULL;
    KASSERT(signal_action(t, SIGKILL) == 0);            // SIGKILL uncatchable
}

// --- virtio block device (Phase 19) ---

static void test_block_present(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init();
    virtio_blk_init();
    KASSERT(block_present());
}

static void test_block_write_read(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init();
    virtio_blk_init();
    static uint8_t w[512], r[512];
    for (int i = 0; i < 512; i++) { w[i] = (uint8_t)(i * 7 + 3); r[i] = 0; }
    KASSERT(block_write(1, w) == 0);
    KASSERT(block_read(1, r) == 0);
    for (int i = 0; i < 512; i++) { KASSERT(r[i] == w[i]); }
}

static void test_block_two_sectors(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init();
    virtio_blk_init();
    static uint8_t a[512], b[512], ra[512], rb[512];
    for (int i = 0; i < 512; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)(255 - i); }
    KASSERT(block_write(2, a) == 0);
    KASSERT(block_write(3, b) == 0);
    KASSERT(block_read(2, ra) == 0);
    KASSERT(block_read(3, rb) == 0);
    for (int i = 0; i < 512; i++) { KASSERT(ra[i] == a[i] && rb[i] == b[i]); }
}

// --- on-disk filesystem (Phase 20) ---

static void test_sfs_create_write_read(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init(); sfs_mkfs();
    struct vnode *r = sfs_mount();
    KASSERT(r && r->type == VN_DIR);
    struct vnode *f = r->ops->create(r, "f", VN_FILE);
    KASSERT(f != 0);
    struct file fh = { .vnode = f, .off = 0 };
    KASSERT(vfs_write(&fh, "hello", 5) == 5);
    char b[8] = {0};
    struct file fr = { .vnode = f, .off = 0 };
    KASSERT(vfs_read(&fr, b, 8) == 5);
    KASSERT(b[0] == 'h' && b[4] == 'o');
    KASSERT(f->size == 5);
}

static void test_sfs_persists_remount(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init(); sfs_mkfs();
    struct vnode *r = sfs_mount();
    struct vnode *f = r->ops->create(r, "p", VN_FILE);
    struct file fh = { .vnode = f, .off = 0 };
    vfs_write(&fh, "persist", 7);
    struct vnode *r2 = sfs_mount();                  // fresh vnodes from disk
    struct vnode *f2 = r2->ops->lookup(r2, "p");
    KASSERT(f2 != 0);
    char b[8] = {0};
    struct file fr = { .vnode = f2, .off = 0 };
    KASSERT(vfs_read(&fr, b, 8) == 7);
    KASSERT(b[0] == 'p' && b[6] == 't');
}

static void test_sfs_readdir(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init(); sfs_mkfs();
    struct vnode *r = sfs_mount();
    r->ops->create(r, "aa", VN_FILE);
    r->ops->create(r, "bb", VN_FILE);
    char n[32]; int seen = 0;
    for (int i = 0; r->ops->readdir(r, i, n) == 0; i++) {
        if (n[0] == 'a' && n[1] == 'a') { seen |= 1; }
        if (n[0] == 'b' && n[1] == 'b') { seen |= 2; }
    }
    KASSERT(seen == 3);
}

static void test_sfs_multiblock(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init(); sfs_mkfs();
    struct vnode *r = sfs_mount();
    struct vnode *f = r->ops->create(r, "big", VN_FILE);
    static uint8_t w[600], rb[600];
    for (int i = 0; i < 600; i++) { w[i] = (uint8_t)(i * 3 + 1); rb[i] = 0; }
    struct file fh = { .vnode = f, .off = 0 };
    KASSERT(vfs_write(&fh, w, 600) == 600);          // spans direct[0] and direct[1]
    struct file fr = { .vnode = f, .off = 0 };
    KASSERT(vfs_read(&fr, rb, 600) == 600);
    for (int i = 0; i < 600; i++) { KASSERT(rb[i] == w[i]); }
}

static void test_vfs_mount_at(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    sfs_mkfs();
    vfs_mount_at("/disk", sfs_mount());
    struct vnode *d = vfs_lookup("/disk");
    KASSERT(d && d->type == VN_DIR);                 // the mounted FS root
    struct vnode *f = vfs_create("/disk/x", VN_FILE); // create routes into SFS
    KASSERT(f != 0);
    KASSERT(vfs_lookup("/disk/x") != 0);             // and is found there
    KASSERT(vfs_lookup("/disk/nope") == 0);
}

// --- virtio-net (Phase 21) ---

static void test_net_present(void)
{
    pmm_init(); kheap_init(); virtio_net_init();
    KASSERT(net_present());
    uint8_t m[6]; net_mac(m);
    KASSERT(m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);   // non-zero MAC
}

static void test_net_arp_roundtrip(void)
{
    pmm_init(); kheap_init(); virtio_net_init();
    uint8_t mac[6]; net_mac(mac);

    // Hand-build an ARP request: who-has 10.0.2.2, tell 10.0.2.15.
    uint8_t f[42];
    for (int i = 0; i < 6; i++) { f[i] = 0xff; }        // dst: broadcast
    for (int i = 0; i < 6; i++) { f[6 + i] = mac[i]; }  // src: us
    f[12] = 0x08; f[13] = 0x06;                          // ethertype = ARP
    uint8_t arp[28] = { 0,1, 8,0, 6,4, 0,1,              // eth/ipv4, opcode request
        mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], 10,0,2,15,   // sender ha/ip
        0,0,0,0,0,0, 10,0,2,2 };                          // target ha/ip
    for (int i = 0; i < 28; i++) { f[14 + i] = arp[i]; }
    KASSERT(net_send(f, 42) == 0);

    // Poll for the gateway's ARP reply (opcode 2, sender IP 10.0.2.2). QEMU
    // injects the reply from its host-side network thread, so we may spin a while.
    // ARP fields: [6..7]=opcode, [14..17]=sender protocol (IP) address.
    uint8_t r[1600];
    int got = 0;
    for (long tries = 0; tries < 200000000L && !got; tries++) {
        __asm__ volatile("dsb sy" ::: "memory");
        int n = net_recv(r, sizeof(r));
        if (n >= 42 && r[12] == 0x08 && r[13] == 0x06 &&
            r[14 + 7] == 2 &&                            // ARP opcode = reply
            r[14 + 14] == 10 && r[14 + 15] == 0 && r[14 + 16] == 2 && r[14 + 17] == 2) {
            got = 1;                                     // sender IP = 10.0.2.2
        }
    }
    KASSERT(got);
}

// --- TCP/IP stack (Phase 22) ---

static void test_inet_checksum(void)
{
    uint8_t d[20] = {0x45,0,0,0x1c, 0,0,0,0, 0x40,1,0,0, 10,0,2,15, 10,0,2,2};
    uint16_t c = inet_csum(d, 20);
    d[10] = (uint8_t)(c >> 8); d[11] = (uint8_t)(c & 0xff);   // place it in the csum field
    KASSERT(inet_csum(d, 20) == 0);                            // now the header verifies as 0
}

static void test_arp_resolve(void)
{
    pmm_init(); kheap_init(); vm_init(); virtio_net_init(); net_stack_init();
    uint8_t mac[6];
    KASSERT(arp_resolve(IP_GATEWAY, mac) == 0);               // 10.0.2.2 -> gateway MAC
    KASSERT(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
}

static void test_icmp_ping(void)
{
    pmm_init(); kheap_init(); vm_init(); virtio_net_init(); net_stack_init();
    int ms = -1;
    KASSERT(net_ping(IP_GATEWAY, &ms) == 0);                  // gateway answers echo
}

// --- DNS encode/decode (Phase 22, pure -- no network) ---

static void test_dns_build_query(void)
{
    uint8_t q[512];
    int n = dns_build_query(q, 0x1234, "a.bc");
    // 12 header + [1]'a' + [2]'bc' + [0] root (6) + 4 (qtype/qclass) = 22 bytes
    KASSERT(n == 22);
    KASSERT(q[0] == 0x12 && q[1] == 0x34);     // id
    KASSERT(q[2] == 0x01 && q[3] == 0x00);     // flags: recursion desired
    KASSERT(q[4] == 0 && q[5] == 1);           // QDCOUNT = 1
    KASSERT(q[12] == 1 && q[13] == 'a');       // label "a"
    KASSERT(q[14] == 2 && q[15] == 'b' && q[16] == 'c');  // label "bc"
    KASSERT(q[17] == 0);                       // root label
    KASSERT(q[18] == 0 && q[19] == 1);         // QTYPE = A
    KASSERT(q[20] == 0 && q[21] == 1);         // QCLASS = IN
}

static void test_dns_parse_a_record(void)
{
    // A response to a query for "a" with one A record 1.2.3.4, the answer name
    // given as a compression pointer (0xC0 0x0C) back to the question.
    uint8_t m[] = {
        0xAB,0xCD, 0x81,0x80, 0,1, 0,1, 0,0, 0,0,   // header: 1 question, 1 answer
        1,'a',0, 0,1, 0,1,                          // question: "a" A IN
        0xC0,0x0C, 0,1, 0,1, 0,0,0,60, 0,4, 1,2,3,4 // answer: ptr, A, IN, ttl, rdlen 4, IP
    };
    uint32_t ip = 0;
    KASSERT(dns_parse_answer(m, (int)sizeof(m), 0xABCD, &ip) == 0);
    KASSERT(ip == 0x01020304u);                 // 1.2.3.4
    KASSERT(dns_parse_answer(m, (int)sizeof(m), 0x0000, &ip) == -1);  // id mismatch
}

static void test_dns_parse_skips_cname(void)
{
    // Two answers: a CNAME (type 5) then the real A record. The parser must skip
    // the CNAME by its RDLENGTH and return the A record's address.
    uint8_t m[] = {
        0xAB,0xCD, 0x81,0x80, 0,1, 0,2, 0,0, 0,0,   // 1 question, 2 answers
        1,'a',0, 0,1, 0,1,                          // question
        0xC0,0x0C, 0,5, 0,1, 0,0,0,60, 0,2, 0xC0,0x0C, // answer 1: CNAME, rdlen 2
        0xC0,0x0C, 0,1, 0,1, 0,0,0,60, 0,4, 9,8,7,6    // answer 2: A 9.8.7.6
    };
    uint32_t ip = 0;
    KASSERT(dns_parse_answer(m, (int)sizeof(m), 0xABCD, &ip) == 0);
    KASSERT(ip == 0x09080706u);                 // 9.8.7.6
}

static void test_dns_resolve_live(void)
{
    pmm_init(); kheap_init(); vm_init(); virtio_net_init(); net_stack_init();
    uint32_t ip = 0;
    // SLIRP's resolver (10.0.2.3) forwards to the host's nameserver.
    KASSERT(net_resolve("example.com", &ip) == 0);
    KASSERT(ip != 0);
    kprintf("    [resolved example.com -> %u.%u.%u.%u]\n",
            (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
}

static void test_dns_parse_rcode_error(void)
{
    // Header RCODE = 3 (NXDOMAIN) -> parse fails even though counts look sane.
    uint8_t m[] = {
        0xAB,0xCD, 0x81,0x83, 0,1, 0,0, 0,0, 0,0,   // flags low nibble = 3
        1,'a',0, 0,1, 0,1,
    };
    uint32_t ip = 0;
    KASSERT(dns_parse_answer(m, (int)sizeof(m), 0xABCD, &ip) == -1);
}

// The registry of all tests.
static const struct ktest tests[] = {
    { "pmm: pages aligned & contiguous", test_pmm_aligned_and_contiguous },
    { "pmm: freed page reused",          test_pmm_free_reuse },
    { "pmm: alloc_pages contiguous run", test_pmm_alloc_pages_contiguous },
    { "kheap: alloc write/read",         test_kheap_write_read },
    { "kheap: freed block reused",       test_kheap_free_reuse },
    { "kheap: coalesce adjacent blocks", test_kheap_coalesce },
    { "thread: create sets up context",  test_thread_create_context },
    { "sched: round-robin order",         test_round_robin_order },
    { "sched: time slice expiry",         test_time_slice_expiry },
    { "sched: priority order",            test_priority_order },
    { "sched: sleep wakes after ticks",   test_sleep_wakes_after_ticks },
    { "sched: block wakes on channel",    test_block_wakes_on_channel },
    { "sched: block woken by signal",     test_block_woken_by_signal },
    { "sched: wait_event times out",      test_wait_event_times_out },
    { "sched: wait_event early wake",     test_wait_event_early_wake },
    { "console: ring is FIFO",            test_console_ring_fifo },
    { "console: Ctrl-C signals fg",       test_console_ctrlc_signals_foreground },
    { "syscall: write returns len",       test_syscall_write_returns_len },
    { "syscall: unknown returns -1",      test_syscall_unknown },
    { "syscall: yield returns 0",         test_syscall_yield },
    { "syscall: result written to x0",    test_syscall_return_in_x0 },
    { "syscall: getpid returns id",       test_syscall_getpid },
    { "syscall: sleep blocks N ticks",    test_syscall_sleep_blocks },
    { "syscall: exit ends thread",        test_syscall_exit_ends_thread },
    { "vm: user data is private",         test_as_data_is_private },
    { "vm: image maps code",              test_as_image_maps_code },
    { "vm: kernel map is shared",         test_as_kernel_shared },
    { "vm: unmapped VA -> 0",             test_as_unmapped_returns_zero },
    { "vm: user stack is private",        test_as_stack_is_private },
    { "vfs: mount root is dir",           test_vfs_mount_root_is_dir },
    { "vfs: create and lookup",           test_vfs_create_and_lookup },
    { "vfs: write then read",             test_vfs_write_then_read },
    { "vfs: read at offset",              test_vfs_read_offset },
    { "vfs: write grows file",            test_vfs_write_grows },
    { "vfs: readdir lists entries",       test_vfs_readdir_lists },
    { "vfs: lookup missing -> null",      test_vfs_lookup_missing },
    { "vfs: nested directory",            test_vfs_nested_dir },
    { "initrd: unpacks files",            test_initrd_unpacked },
    { "fd: open returns fd",              test_fd_open_returns_fd },
    { "fd: read syscall",                 test_fd_read_syscall },
    { "fd: open missing -> -1",           test_fd_open_missing },
    { "fd: close then reuse",             test_fd_close_reuse },
    { "cow: clone shares pages",          test_cow_clone_shares_pages },
    { "cow: clone refcount 2",            test_cow_clone_refcount },
    { "cow: fault copies page",           test_cow_fault_copies },
    { "cow: fault drops refcount",        test_cow_fault_refcount },
    { "cow: fault on non-cow -> 0",       test_cow_fault_non_cow },
    { "syscall: readdir lists dir",       test_syscall_readdir },
    { "asid: assigned nonzero",           test_asid_assigned_nonzero },
    { "asid: unique per space",           test_asid_unique },
    { "asid: clone gets own asid",        test_asid_clone_distinct },
    { "asid: user page is non-global",    test_asid_user_page_nonglobal },
    { "asid: rollover recycles",          test_asid_rollover_recycles },
    { "asid: free recycles",              test_asid_free_recycles },
    { "proc: as_destroy frees pages",     test_as_destroy_frees_pages },
    { "proc: as_destroy recycles asid",   test_as_destroy_recycles_asid },
    { "proc: wait reaps child + status",  test_wait_reaps_child },
    { "proc: wait with no children -> -1",test_wait_no_children },
    { "proc: exec missing path -> -1",    test_exec_missing_returns_neg1 },
    { "proc: exec sets up argv on stack", test_exec_argv_on_stack },
    { "elf: map_segment zeroes bss",      test_as_map_segment_bss_zeroed },
    { "elf: rejects bad magic",           test_elf_rejects_bad_magic },
    { "elf: loads entry + segment",       test_elf_entry_and_segment },
    { "elf: as_create_elf maps stack",    test_as_create_elf_has_stack },
    { "mem: sbrk grows + maps + zeroes",  test_sbrk_grows_and_maps },
    { "mem: sbrk spans multiple pages",   test_sbrk_multi_page },
    { "mem: mmap maps zeroed page",       test_mmap_maps_zeroed },
    { "mem: mmap returns distinct pages", test_mmap_two_distinct },
    { "mem: munmap unmaps",               test_munmap_unmaps },
    { "shm: create returns handle",       test_shm_create_handle },
    { "shm: maps shared across spaces",   test_shm_shared_pages },
    { "shm: survives a mapper exiting",   test_shm_survives_unmap },
    { "pipe: write then read",            test_pipe_write_then_read },
    { "pipe: read EOF when no writers",   test_pipe_eof },
    { "pipe: write -1 when no readers",   test_pipe_broken },
    { "pipe: ring buffer wraps",          test_pipe_wraps },
    { "file: refcount dup/close",         test_file_refcount },
    { "sig: kill sets pending",           test_kill_sets_pending },
    { "sig: kill by pid",                 test_kill_by_pid },
    { "sig: default vs handler action",   test_sig_default_vs_handler },
    { "block: disk present",              test_block_present },
    { "block: write then read sector",    test_block_write_read },
    { "block: two sectors independent",   test_block_two_sectors },
    { "sfs: create write read",           test_sfs_create_write_read },
    { "sfs: persists across remount",     test_sfs_persists_remount },
    { "sfs: readdir lists entries",       test_sfs_readdir },
    { "sfs: multi-block file",            test_sfs_multiblock },
    { "vfs: mount at /disk",              test_vfs_mount_at },
    { "net: present + MAC",               test_net_present },
    { "net: ARP round-trip",              test_net_arp_roundtrip },
    { "net: internet checksum",           test_inet_checksum },
    { "net: ARP resolve gateway",         test_arp_resolve },
    { "net: ICMP ping gateway",           test_icmp_ping },
    { "dns: build A query",               test_dns_build_query },
    { "dns: parse A record",              test_dns_parse_a_record },
    { "dns: skip CNAME to A",             test_dns_parse_skips_cname },
    { "dns: RCODE error -> fail",         test_dns_parse_rcode_error },
    { "dns: resolve localhost (live)",    test_dns_resolve_live },
};

int run_self_tests(void)
{
    return ktest_run(tests, sizeof(tests) / sizeof(tests[0]));
}
