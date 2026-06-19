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
#include "errno.h"
#include "vm.h"
#include "vfs.h"
#include "ramfs.h"
#include "proc.h"
#include "elf.h"
#include "shm.h"
#include "pipe.h"
#include "pty.h"
#include "signal.h"
#include "block.h"
#include "ext2.h"
#include "virtio.h"
#include "input.h"
#include "gfx.h"
#include "seat.h"
#include "rd.h"
#include "net.h"
#include "console.h"
#include "socket.h"
#include "tcp.h"
#include "tcp_reasm.h"
#include "tcp_rto.h"
#include "tcp_cc.h"
#include "poll.h"
#include "lm.h"

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

// A blocked pipe reader must SLEEP (THREAD_BLOCKED), not spin -- otherwise it
// monopolises the CPU with IRQs masked and starves the timer/device interrupts a
// sleeping peer needs to wake (the `http | wc` deadlock).
static struct pipe *pwt_pipe;
static struct thread *pwt_reader;
static int pwt_got;
static char pwt_buf[8];
static void pwt_reader_fn(void *a)
{
    (void)a;
    struct file f = { .pipe = pwt_pipe, .ref = 1 };   // a read end on the pipe
    pwt_got = pipe_read(&f, pwt_buf, sizeof(pwt_buf));
}
static void test_pipe_read_blocks_not_spins(void)
{
    pmm_init(); kheap_init(); sched_init();
    pwt_pipe = pipe_alloc();                 // readers=1, writers=1
    pwt_got = -99;
    pwt_reader = thread_create(pwt_reader_fn, 0, 1);

    yield();                                 // reader runs; empty pipe -> blocks
    KASSERT(pwt_got == -99);                          // hasn't returned
    KASSERT(pwt_reader->state == THREAD_BLOCKED);     // asleep, NOT spinning

    struct file wf = { .pipe = pwt_pipe, .writable = 1, .ref = 1 };
    pipe_write(&wf, "hi", 2);                 // feeding it must wake the reader
    yield();
    KASSERT(pwt_got == 2);
    KASSERT(pwt_buf[0] == 'h' && pwt_buf[1] == 'i');
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

// Ctrl-C signals the terminal's FOREGROUND PROCESS GROUP (set via tcsetpgrp/
// TIOCSPGRP) -- so a whole job dies, even one a job-control shell put in its own
// group. This is the kernel half of the fix for C-c not interrupting a command
// run inside busybox sh in the frame.
static void test_tty_intr_signals_fg_pgrp(void)
{
    pmm_init(); kheap_init(); sched_init();
    struct thread *job   = thread_create(noop_worker, 0, 1);  // pgid defaults to its id
    struct thread *other = thread_create(noop_worker, 0, 1);  // a different group
    sched_set_foreground(0);               // force the GROUP path, not the thread fallback
    tty_set_fg_pgrp(job->pgid);            // terminal foreground group = the job's
    console_input(3);                      // Ctrl-C -> tty_intr -> kill(-fg_pgrp)
    KASSERT(job->sig_pending & (1ull << SIGINT));       // the fg group got SIGINT
    KASSERT(!(other->sig_pending & (1ull << SIGINT)));  // an unrelated group did NOT
    tty_set_fg_pgrp(0);                    // reset the shared tty state for later tests
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
    KASSERT((long)tf.x[0] == -ENOSYS);   // Linux convention for an unknown call
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

// ioctl: terminal requests -- the gate that makes ash go interactive.
// musl's isatty(fd) calls ioctl(fd, TCGETS, &termios_buf); a return of 0
// means the fd is a terminal. Without that, ash treats stdin as a pipe and
// runs non-interactively (no prompt). We verify both sides:
//   - TCGETS on fd 0 succeeds (-> isatty(0) == true)
//   - An unknown request still returns -ENOTTY (musl falls back gracefully)
static void test_ioctl_tcgets_is_tty(void)
{
    pmm_init(); kheap_init();
    // Stack buffer for the kernel to write the termios struct into; the exact
    // contents do not matter for isatty() -- only the zero return does.
    char termios_buf[64];
    struct trapframe tf;
    tf.x[8] = SYS_IOCTL; tf.x[0] = 0; tf.x[1] = TCGETS; tf.x[2] = (uint64_t)(uintptr_t)termios_buf;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == 0);                 // success -> isatty(0) true
    tf.x[8] = SYS_IOCTL; tf.x[0] = 0; tf.x[1] = 0xDEAD; tf.x[2] = 0;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == -ENOTTY);           // unknown request still ENOTTY
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

// O_TRUNC / re-save: truncate resets size to 0, and a shorter rewrite leaves
// NO stale tail (the bug where C-x C-s kept the old text past the new length).
static void test_vfs_truncate(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    vfs_create("/f", VN_FILE);
    struct file *w = vfs_open("/f");
    vfs_write(w, "LONG-OLD-CONTENT", 16);
    vfs_close(w);
    struct vnode *vn = vfs_lookup("/f");
    KASSERT(vn->size == 16);
    vfs_truncate(vn);                       // O_TRUNC
    KASSERT(vn->size == 0);
    struct file *w2 = vfs_open("/f");
    vfs_write(w2, "NEW", 3);                // shorter rewrite
    vfs_close(w2);
    KASSERT(vn->size == 3);                 // size shrank -- no stale 16 bytes
    struct file *r = vfs_open("/f");
    char buf[20] = {0};
    KASSERT(vfs_read(r, buf, 16) == 3);     // only the new 3 bytes are readable
    KASSERT(bytes_eq(buf, "NEW", 3));
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

// A path without a leading '/' is resolved relative to the root. MyOSv2 has no
// per-process current directory, so "relative" can only mean "from root" -- and
// that is exactly what lets the shell accept `cat hello.txt`, not just the more
// awkward `cat /hello.txt`. A relative path must therefore resolve to the very
// same vnode as its absolute twin.
static void test_vfs_lookup_relative(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    vfs_create("/a.txt", VN_FILE);
    vfs_create("/d", VN_DIR);
    vfs_create("/d/f.txt", VN_FILE);

    KASSERT(vfs_lookup("a.txt") != 0);                          // found at all
    KASSERT(vfs_lookup("a.txt")   == vfs_lookup("/a.txt"));     // == absolute twin
    KASSERT(vfs_lookup("d/f.txt") == vfs_lookup("/d/f.txt"));   // nested, too
    KASSERT(vfs_lookup("")        == vfs_root());               // empty == root
    KASSERT(vfs_lookup("nope")    == 0);                        // still misses cleanly
}

// Seed a fresh in-RAM filesystem for the VFS/syscall KTESTs: a ramfs root with
// /hello.txt = "Hello, MyOSv2!\n". Replaces the old initrd_unpack() fixture now
// that the real userland lives on the ext2 disk image, not embedded in the
// kernel. Self-contained so these tests never depend on production seed code.
static void fs_fresh(void)
{
    vfs_mount_root(ramfs_type());
    struct vnode *vn = vfs_create("/hello.txt", VN_FILE);
    struct file f = { .vnode = vn, .off = 0 };
    vfs_write(&f, "Hello, MyOSv2!\n", 15);
}

static void test_seed_fs_provides_hello(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    struct file *f = vfs_open("/hello.txt");
    KASSERT(f != 0);
    char buf[16] = {0};
    int n = vfs_read(f, buf, 14);             // 14 = the content, minus the trailing newline
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
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/hello.txt"; tf.x[2] = 0;
    do_syscall(&tf);
    fd_res = (long)tf.x[0];
}
static void test_fd_open_returns_fd(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
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
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/hello.txt"; tf.x[2] = 0;
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
    fs_fresh();
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
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/nope"; tf.x[2] = 0;
    do_syscall(&tf);
    fd_miss = (long)tf.x[0];
}
static void test_fd_open_missing(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    fd_miss = 0;
    sched_init();
    thread_create(fd_miss_worker, 0, 1);
    while (fd_miss == 0) { yield(); }
    KASSERT(fd_miss < 0);                 // -ENOENT now (Linux negative-errno)
}

static long fd_a, fd_b;
static void fd_reuse_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/hello.txt"; tf.x[2] = 0;
    do_syscall(&tf); fd_a = (long)tf.x[0];
    tf.x[8] = SYS_CLOSE; tf.x[0] = (uint64_t)fd_a; do_syscall(&tf);
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/hello.txt"; tf.x[2] = 0;
    do_syscall(&tf); fd_b = (long)tf.x[0];
}
static void test_fd_close_reuse(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    fd_a = fd_b = -1;
    sched_init();
    thread_create(fd_reuse_worker, 0, 1);
    while (fd_b == -1) { yield(); }
    KASSERT(fd_a == fd_b);
}

// cwd: chdir/getcwd + relative paths resolve against the process cwd.
static int cwd_ok;
static void cwd_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    char buf[64];
    int ok = 1;
    tf.x[8] = SYS_GETCWD; tf.x[0] = (uint64_t)(uintptr_t)buf; tf.x[1] = 64;
    do_syscall(&tf); ok = ok && bytes_eq(buf, "/", 2);              // starts at root
    tf.x[8] = SYS_CHDIR; tf.x[0] = (uint64_t)(uintptr_t)"/a";
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == 0);
    tf.x[8] = SYS_GETCWD; tf.x[0] = (uint64_t)(uintptr_t)buf; tf.x[1] = 64;
    do_syscall(&tf); ok = ok && bytes_eq(buf, "/a", 3);
    tf.x[8] = SYS_CHDIR; tf.x[0] = (uint64_t)(uintptr_t)"b";        // relative
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == 0);
    tf.x[8] = SYS_GETCWD; tf.x[0] = (uint64_t)(uintptr_t)buf; tf.x[1] = 64;
    do_syscall(&tf); ok = ok && bytes_eq(buf, "/a/b", 5);
    tf.x[8] = SYS_CHDIR; tf.x[0] = (uint64_t)(uintptr_t)"..";       // pops a component
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == 0);
    tf.x[8] = SYS_GETCWD; tf.x[0] = (uint64_t)(uintptr_t)buf; tf.x[1] = 64;
    do_syscall(&tf); ok = ok && bytes_eq(buf, "/a", 3);
    // a relative create lands in the cwd: "x" -> /a/x
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"x"; tf.x[2] = O_CREAT; tf.x[3] = 0;
    do_syscall(&tf); ok = ok && ((long)tf.x[0] >= 0);
    ok = ok && (vfs_lookup("/a/x") != 0);
    tf.x[8] = SYS_CHDIR; tf.x[0] = (uint64_t)(uintptr_t)"/nope";    // missing -> ENOENT
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == -ENOENT);
    tf.x[8] = SYS_CHDIR; tf.x[0] = (uint64_t)(uintptr_t)"/a/f";     // a file -> ENOTDIR
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == -ENOTDIR);
    cwd_ok = ok ? 1 : -1;
}
static void test_cwd_chdir_getcwd(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    vfs_create("/a", VN_DIR);
    vfs_create("/a/b", VN_DIR);
    vfs_create("/a/f", VN_FILE);
    cwd_ok = 0;
    sched_init();
    thread_create(cwd_worker, 0, 1);
    while (cwd_ok == 0) { yield(); }
    KASSERT(cwd_ok == 1);
}

// getdents64: open a directory fd and read packed linux_dirent64 records.
static uint8_t gd_buf[512];
static long gd_n;
static int gd_count, gd_found_two;
static void gd_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/"; tf.x[2] = 0;
    do_syscall(&tf);
    long fd = (long)tf.x[0];
    tf.x[8] = SYS_GETDENTS64; tf.x[0] = (uint64_t)fd;
    tf.x[1] = (uint64_t)(uintptr_t)gd_buf; tf.x[2] = sizeof(gd_buf);
    do_syscall(&tf);
    gd_n = (long)tf.x[0];
    // Walk the packed records: each one's d_reclen (u16 @16) is the stride.
    long off = 0;
    while (off + 19 <= gd_n) {
        const char *nm = (const char *)(gd_buf + off + 19);
        if (nm[0] == 't' && nm[1] == 'w' && nm[2] == 'o' && nm[3] == 0) { gd_found_two = 1; }
        gd_count++;
        off += *(uint16_t *)(gd_buf + off + 16);
    }
}
static void test_getdents64_lists(void)
{
    pmm_init(); kheap_init();
    vfs_mount_root(ramfs_type());
    vfs_create("/one", VN_FILE);
    vfs_create("/two", VN_FILE);
    vfs_create("/three", VN_FILE);
    gd_n = -1; gd_count = 0; gd_found_two = 0;
    sched_init();
    thread_create(gd_worker, 0, 1);
    while (gd_n == -1) { yield(); }
    KASSERT(gd_n > 0);
    KASSERT(gd_count == 3);          // all three entries packed into the buffer
    KASSERT(gd_found_two);           // d_name round-trips correctly
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
    fs_fresh();
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
    uint64_t argv_arr = 0;
    uint64_t sp = proc_setup_argv(as, argv, &argc, &argv_arr);
    KASSERT(argc == 2);
    KASSERT((sp & 15) == 0);                       // AArch64 needs a 16-aligned sp

    // The Linux/aarch64 initial stack: sp -> argc, argv[], NULL, envp NULL, auxv.
    uint64_t pbase = USER_STACK_TOP - 0x1000;      // VA of the top stack page
    uint8_t *page = (uint8_t *)(uintptr_t)as_translate(as, pbase);
    KASSERT(stack_u64(page, pbase, sp) == 2);          // argc at sp
    KASSERT(argv_arr == sp + 8);                       // argv[] right after argc
    uint64_t a0 = stack_u64(page, pbase, sp + 8);      // argv[0]
    uint64_t a1 = stack_u64(page, pbase, sp + 16);     // argv[1]
    KASSERT(stack_u64(page, pbase, sp + 24) == 0);     // argv[argc] = NULL
    KASSERT(stack_u64(page, pbase, sp + 32) == 0);     // envp[0]   = NULL
    KASSERT(stack_u64(page, pbase, sp + 40) == 6);     // AT_PAGESZ
    KASSERT(stack_u64(page, pbase, sp + 48) == 4096);  //   = page size
    KASSERT(stack_u64(page, pbase, sp + 56) == 25);    // AT_RANDOM
    uint64_t rnd = stack_u64(page, pbase, sp + 64);    //   -> 16 bytes in the page
    KASSERT(rnd >= pbase && rnd < USER_STACK_TOP);
    KASSERT(stack_u64(page, pbase, sp + 72) == 0);     // AT_NULL terminates auxv

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

// --- virtio-input (Phase 25.1) ---

static void test_input_devices_present(void)
{
    // The graphical machine needs BOTH a keyboard and a tablet. They are two
    // separate virtio devices with the same DeviceID (18), so the transport
    // needs to enumerate beyond the first match.
    KASSERT(virtio_find_nth(18, 0) != 0);
    KASSERT(virtio_find_nth(18, 1) != 0);
    KASSERT(virtio_find_nth(18, 2) == 0);   // there is no third one
}

static void test_input_present(void)
{
    pmm_init(); kheap_init();
    input_init();
    KASSERT(input_present());
}

static void test_input_poll_drain(void)
{
    // Fake a completed event in device 0's used ring, exactly as the device
    // would leave it, and check input_poll_event() hands it to us and that the
    // buffer is recycled (the queue never starves).
    pmm_init(); kheap_init();
    input_init();
    struct input_event ev = { 0xFFFF, 0xFFFF, 0xFFFFFFFF };
    KASSERT(input_poll_event(&ev) == 0);            // nothing pending
    input_test_inject(0, EV_KEY, 30 /*KEY_A*/, 1);
    KASSERT(input_poll_event(&ev) == 1);
    KASSERT(ev.type == EV_KEY && ev.code == 30 && ev.value == 1);
    KASSERT(input_poll_event(&ev) == 0);            // consumed
}

// --- rd_core: the redisplay engine (Phase 25.3) ---
//
// Like the Lisp core, rd_core is portable C dual-built into the kernel
// exactly so these tests can red-green layout, damage and painting on target.

static char rdbuf_store[512];
static struct rd_cell rd_front[160 * 45], rd_back[160 * 45];

static void test_rd_gap_buffer(void)
{
    struct rd_buffer b;
    rd_buf_init(&b, "*scratch*", rdbuf_store, sizeof(rdbuf_store));
    KASSERT(rd_buf_len(&b) == 0);
    rd_buf_insert(&b, "hello");
    KASSERT(rd_buf_len(&b) == 5);
    KASSERT(rd_buf_char_at(&b, 0) == 'h' && rd_buf_char_at(&b, 4) == 'o');
    // Move the point into the middle (forces a gap move) and edit there.
    rd_buf_set_point(&b, 2);
    rd_buf_insert(&b, "XY");
    KASSERT(rd_buf_len(&b) == 7);
    KASSERT(rd_buf_char_at(&b, 1) == 'e' && rd_buf_char_at(&b, 2) == 'X');
    KASSERT(rd_buf_char_at(&b, 3) == 'Y' && rd_buf_char_at(&b, 4) == 'l');
    rd_buf_delete(&b, 2);                          // delete "XY" again
    KASSERT(rd_buf_len(&b) == 5);
    KASSERT(rd_buf_char_at(&b, 2) == 'l');
    KASSERT(rd_buf_char_at(&b, 99) == -1);
}

// A 640x320 frame in RD_CELL_W x RD_CELL_H cells: the single window's last
// row is its modeline (rows-2 of the frame); the frame's last row is the echo
// area. Geometry is computed from the cell constants so font changes don't
// invalidate the tests.
static struct rd_frame rdf;
static struct rd_buffer rdb, rdb2;
static char rdb_store[512], rdb2_store[512];

static void rd_fresh(void)
{
    rd_buf_init(&rdb, "*scratch*", rdb_store, sizeof(rdb_store));
    rd_buf_init(&rdb2, "*other*", rdb2_store, sizeof(rdb2_store));
    rd_frame_init(&rdf, 640, 320, rd_front, rd_back, &rdb);
}

static void test_rd_single_window_layout(void)
{
    rd_fresh();
    rd_buf_insert(&rdb, "hi");
    rd_echo(&rdf, "ok");
    rd_layout(&rdf);
    KASSERT(rdf.cols == 640 / RD_CELL_W && rdf.rows == 320 / RD_CELL_H);
    KASSERT(rd_cell_at(&rdf, 0, 0)->ch == 'h');
    KASSERT(rd_cell_at(&rdf, 1, 0)->ch == 'i');
    KASSERT(rd_cell_at(&rdf, 2, 0)->ch == ' ');
    // The modeline (the window's last row) carries the buffer name in face 1.
    int ml = rdf.rows - 2, echo_row = rdf.rows - 1;
    int found = 0;
    for (int c = 0; c < rdf.cols - 9; c++) {
        if (rd_cell_at(&rdf, c, ml)->ch == '*' &&
            rd_cell_at(&rdf, c + 1, ml)->ch == 's') { found = 1; }
    }
    KASSERT(found);
    KASSERT(rd_cell_at(&rdf, 0, ml)->face == 1);
    // Echo area on the frame's last row, default face.
    KASSERT(rd_cell_at(&rdf, 0, echo_row)->ch == 'o' && rd_cell_at(&rdf, 1, echo_row)->ch == 'k');
    // A long line truncates at the window edge (no wrap in v1).
    rd_buf_set_point(&rdb, rd_buf_len(&rdb));
    for (int i = 0; i < 30; i++) { rd_buf_insert(&rdb, "0123456789"); }
    rd_layout(&rdf);
    KASSERT(rd_cell_at(&rdf, rdf.cols - 1, 0)->ch != ' ');   // filled to the edge...
    KASSERT(rd_cell_at(&rdf, 0, 1)->ch == ' ');    // ...but never wrapped
}

static void test_rd_line_wrap(void)
{
    // With the line-wrap minor mode ON, a logical line longer than the window
    // continues on the next screen row(s) instead of being clipped at the edge.
    rd_fresh();
    // Insert one logical line of 'x's, wider than the window (no newline).
    int over = rdf.cols + 5;
    for (int i = 0; i < over; i++) { rd_buf_insert(&rdb, "x"); }
    rdb.wrap = 1;                                  // enable line-wrap minor mode
    rd_layout(&rdf);
    // Row 0 is filled to the window edge with 'x'...
    KASSERT(rd_cell_at(&rdf, rdf.cols - 1, 0)->ch == 'x');
    // ...and the overflow wrapped onto row 1 (vs. truncate, which blanks it).
    KASSERT(rd_cell_at(&rdf, 0, 1)->ch == 'x');
}

static void test_rd_modeline_mode_name(void)
{
    rd_fresh();
    rd_buf_insert(&rdb, "x");
    // Simulate (set-mode-line-name "Lisp Interaction") on the shown buffer.
    rd_scpy(rdb.mode_line, "Lisp Interaction", (int)sizeof(rdb.mode_line));
    rd_layout(&rdf);
    int ml = rdf.rows - 2;   // modeline = window's last row; rows-1 is the minibuffer
    // The modeline must contain the mode name in parentheses: "(Lisp ...".
    int found = 0;
    for (int c = 0; c < rdf.cols - 1; c++) {
        if (rd_cell_at(&rdf, c, ml)->ch == '(' &&
            rd_cell_at(&rdf, c + 1, ml)->ch == 'L') { found = 1; }
    }
    KASSERT(found);
}

static void test_rd_scroll_follows_point(void)
{
    // Emacs-style scrolling: a buffer taller than its window must scroll so
    // point stays visible (top_line is the window-start the layout advances).
    rd_fresh();
    for (int i = 0; i < 20; i++) { rd_buf_insert(&rdb, "x\n"); }  // 20 lines
    rd_buf_set_point(&rdb, rd_buf_len(&rdb));     // point at the end (line 20)
    rd_layout(&rdf);
    int text_rows = rdf.selected->h - 1;          // last window row is modeline
    KASSERT(rdf.selected->top_line == 20 - text_rows + 1);  // scrolled to show end
    KASSERT(rdf.selected->top_line > 0);                    // it really scrolled
    // Move point back to the top of the buffer: the window scrolls back.
    rd_buf_set_point(&rdb, 0);
    rd_layout(&rdf);
    KASSERT(rdf.selected->top_line == 0);
}

static void test_rd_delete_other(void)
{
    // C-x 1: collapse the window tree to the selected window, keeping its
    // buffer; the tree becomes a single leaf that is both root and selected.
    rd_fresh();
    rd_split(&rdf, 0);                 // two windows
    rd_other_window(&rdf);             // select the second
    rd_set_buffer(&rdf, &rdb2);        // give it the other buffer
    rd_delete_other(&rdf);
    KASSERT(rdf.root == rdf.selected); // one window, root == selected
    KASSERT(rdf.selected->leaf);
    KASSERT(rdf.selected->buf == &rdb2);   // the selected buffer survived
    rd_layout(&rdf);                   // and it lays out cleanly
}

static void test_rd_split_below(void)
{
    rd_fresh();
    rd_buf_insert(&rdb, "top");
    struct rd_win *nw = rd_split(&rdf, 0);         // split below
    KASSERT(nw != 0);
    rd_layout(&rdf);
    // Both halves show *scratch* initially; switch the NEW window (selected
    // stays the original) -- select other, give it the other buffer.
    rd_other_window(&rdf);
    rd_set_buffer(&rdf, &rdb2);
    rd_buf_insert(&rdb2, "bottom");
    rd_layout(&rdf);
    KASSERT(rd_cell_at(&rdf, 0, 0)->ch == 't');    // top window at row 0
    // Bottom window starts at row 9 ((20-1)/2 = 9 rows for a, b gets the rest)
    int brow = rdf.selected->y;
    KASSERT(brow > 0);
    KASSERT(rd_cell_at(&rdf, 0, brow)->ch == 'b');
    // Two modelines: last row of each window.
    KASSERT(rd_cell_at(&rdf, 0, rdf.selected->y - 1)->face == 1);   // a's modeline
    KASSERT(rd_cell_at(&rdf, 0, rdf.rows - 2)->face == 1);          // b's modeline
}

static void test_rd_split_right(void)
{
    rd_fresh();
    rd_buf_insert(&rdb, "L");
    rd_split(&rdf, 1);                             // side by side
    rd_other_window(&rdf);
    rd_set_buffer(&rdf, &rdb2);
    rd_buf_insert(&rdb2, "R");
    rd_layout(&rdf);
    KASSERT(rd_cell_at(&rdf, 0, 0)->ch == 'L');
    int bcol = rdf.selected->x;
    KASSERT(bcol >= rdf.cols / 2 - 1 && bcol <= rdf.cols / 2 + 1);
    KASSERT(rd_cell_at(&rdf, bcol, 0)->ch == 'R');
}

static void test_rd_damage_minimal(void)
{
    rd_fresh();
    rd_split(&rdf, 0);
    rd_other_window(&rdf);
    rd_set_buffer(&rdf, &rdb2);
    struct rd_rect rects[RD_MAX_RECTS];
    rd_redisplay(&rdf, 0, 0, rects, RD_MAX_RECTS); // first paint: everything
    // No change -> no damage.
    KASSERT(rd_redisplay(&rdf, 0, 0, rects, RD_MAX_RECTS) == 0);
    // Edit only the bottom buffer: damage stays inside the bottom window's
    // pixel rect (its rows start at selected->y cells).
    rd_buf_insert(&rdb2, "edit");
    int n = rd_redisplay(&rdf, 0, 0, rects, RD_MAX_RECTS);
    KASSERT(n > 0);
    int bottom_top_px = rdf.selected->y * RD_CELL_H;
    for (int i = 0; i < n; i++) {
        KASSERT(rects[i].y >= bottom_top_px);
    }
}

static void test_rd_glyphs_hit_framebuffer(void)
{
    rd_fresh();
    rd_buf_insert(&rdb, "A");
    static uint32_t fb[640 * 320];
    struct rd_rect rects[RD_MAX_RECTS];
    rd_redisplay(&rdf, fb, 640, rects, RD_MAX_RECTS);
    // The 'A' glyph, anti-aliased: count pixels CLOSER to fg than bg (stems
    // saturate to full fg; edges blend between). Corners stay background.
    uint32_t fg = rdf.faces[0].fg, bg = rdf.faces[0].bg;
    int fg_seen = 0;
    for (int y = 0; y < RD_CELL_H; y++) {
        for (int x = 0; x < RD_CELL_W; x++) {
            uint32_t px = fb[y * 640 + x];
            long df = ((long)((px >> 16) & 0xFF) - (long)((fg >> 16) & 0xFF));
            long db = ((long)((px >> 16) & 0xFF) - (long)((bg >> 16) & 0xFF));
            if (df * df < db * db) { fg_seen++; }
        }
    }
    KASSERT(fg_seen > 12);                         // a real glyph, not noise
    KASSERT(fb[0] == bg);                          // corner: pure background
    // Modeline row painted in face 1 (blends sit between its fg and bg, so
    // just check it is no longer the default background).
    int ml_y = (rdf.rows - 2) * RD_CELL_H + 4;
    KASSERT(fb[ml_y * 640 + 4 * RD_CELL_W] != bg);
    // The cursor cell (point at end of "A": cell (1,0)) is painted inverted:
    // its empty pixels are the DEFAULT FOREGROUND color.
    KASSERT(fb[RD_CELL_W + 1] == fg);
}

static void test_shared_mapping_survives_fork(void)
{
    // Regression (found live, Phase 25.6): as_clone COW-demoted EVERYTHING
    // writable, including as_map_phys mappings (shm canvases, the display
    // framebuffer). After a fork the parent's first write went to a private
    // copy -- a VM rendering into a framebuffer the GPU no longer scanned.
    // Contract: PTE_SHARED mappings stay writable and same-PA in both spaces.
    pmm_init(); kheap_init();
    struct addrspace *parent = as_create();
    uint64_t pg = (uint64_t)(uintptr_t)pmm_alloc();
    uint64_t pa[1] = { pg };
    uint64_t va = as_map_phys(parent, pa, 1);
    KASSERT(va != 0);
    KASSERT(as_is_writable(parent, va));
    struct addrspace *child = as_clone(parent);
    KASSERT(as_is_writable(parent, va));            // NOT demoted to COW
    KASSERT(as_is_writable(child, va));
    KASSERT(as_translate(parent, va) == pg);        // both still the SAME page
    KASSERT(as_translate(child, va) == pg);
}

static void test_rd_minibuffer_echo(void)
{
    rd_fresh();
    // Multi-line echo: the area grows to the content, windows shrink above
    // it, and the selected line renders in face 2 (the vertico bar).
    rd_echo(&rdf, "M-x spl\nsplit-below\nsplit-right");
    rd_echo_select(&rdf, 1);
    rd_layout(&rdf);
    int e0 = rdf.rows - 3;
    KASSERT(rdf.root->h == rdf.rows - 3);              // window area shrank
    KASSERT(rd_cell_at(&rdf, 0, e0)->ch == 'M');
    KASSERT(rd_cell_at(&rdf, 0, e0 + 1)->ch == 's');
    KASSERT(rd_cell_at(&rdf, 0, e0 + 1)->face == 2);   // selection bar
    KASSERT(rd_cell_at(&rdf, 0, e0 + 2)->face == 0);
    // Back to a single line: full window area again.
    rd_echo(&rdf, "ready");
    rd_layout(&rdf);
    KASSERT(rdf.root->h == rdf.rows - 1);
    KASSERT(rdf.echo_sel == -1);                       // rd_echo resets the bar
}

static void test_rd_surface_blit(void)
{
    rd_fresh();
    // Turn the (only) buffer into a surface backed by a small canvas; its
    // pixels must land in the window's rect and the rect must be damaged on
    // EVERY redisplay (a program may have drawn since last time).
    static uint32_t canvas[16 * 8];
    for (int i = 0; i < 16 * 8; i++) { canvas[i] = 0x00ABCDEF; }
    rdb.kind = RD_SURFACE;
    rdb.canvas = canvas; rdb.cv_w = 16; rdb.cv_h = 8;
    static uint32_t fb[640 * 320];
    struct rd_rect rects[RD_MAX_RECTS];
    int n = rd_redisplay(&rdf, fb, 640, rects, RD_MAX_RECTS);
    KASSERT(n > 0);
    KASSERT(fb[0] == 0x00ABCDEF && fb[7 * 640 + 15] == 0x00ABCDEF);
    KASSERT(fb[8 * 640 + 0] != 0x00ABCDEF);        // cropped at the canvas edge
    // The modeline below still renders as text cells (face 1).
    KASSERT(rd_cell_at(&rdf, 0, rdf.rows - 2)->face == 1);
    // A second redisplay with no text change still damages the surface rect.
    int n2 = rd_redisplay(&rdf, fb, 640, rects, RD_MAX_RECTS);
    KASSERT(n2 >= 1);
}

// --- virtio-gpu (Phase 25.2) ---

static void test_gpu_present(void)
{
    pmm_init(); kheap_init();
    gfx_init();
    KASSERT(gfx_present());
    KASSERT(gfx_width() == 2560 && gfx_height() == 1440);
}

static void test_gpu_scanout(void)
{
    pmm_init(); kheap_init();
    gfx_init();
    // Drive the full bring-up against the REAL device: per-seat resource +
    // backing + scanout + one flush. Every command must come back
    // RESP_OK_NODATA from QEMU or the calls report failure.
    uint64_t fb = gfx_fb_new();
    KASSERT(fb != 0 && (fb & 0xFFF) == 0);
    KASSERT(gfx_resource_setup(1, fb) == 0);
    KASSERT(gfx_show(1) == 0);
    KASSERT(gfx_flush_rect(0, 0, 64, 64) == 0);
}

static void test_gpu_two_seats(void)
{
    pmm_init(); kheap_init();
    gfx_init();
    // Two clients, two framebuffers, two resources; the scanout can show
    // either, and flushes target whichever is shown.
    uint64_t fb1 = gfx_fb_new(), fb2 = gfx_fb_new();
    KASSERT(fb1 != 0 && fb2 != 0 && fb1 != fb2);
    KASSERT(gfx_resource_setup(1, fb1) == 0);
    KASSERT(gfx_resource_setup(2, fb2) == 0);
    KASSERT(gfx_show(1) == 0);
    KASSERT(gfx_flush_rect(0, 0, 8, 8) == 0);
    KASSERT(gfx_show(2) == 0);
    KASSERT(gfx_flush_rect(0, 0, 8, 8) == 0);
}

static void test_gpu_cursor_plane(void)
{
    pmm_init(); kheap_init();
    gfx_init();
    uint64_t fb = gfx_fb_new();
    KASSERT(fb != 0);
    KASSERT(gfx_resource_setup(1, fb) == 0);
    KASSERT(gfx_show(1) == 0);
    // First move binds the sprite (create + backing + transfer + UPDATE);
    // later moves are single MOVE_CURSOR commands. The device must accept
    // both -- this is what makes a pointer visible over virtio-gpu, where
    // QEMU hides the host cursor and expects a guest cursor plane.
    KASSERT(gfx_cursor_move(10, 10) == 0);
    KASSERT(gfx_cursor_move(640, 360) == 0);
}

static void test_seat_logic(void)
{
    // The multiplexer's bookkeeping, as pure logic: registration is
    // idempotent per pid, the first client gets the screen, switching is
    // explicit, and releasing the active seat hands off to a survivor.
    seat_reset();
    KASSERT(seat_active() == 0 && seat_active_pid() == -1);
    KASSERT(seat_register(7, 0x1000) == 1);
    KASSERT(seat_register(7, 0x1000) == 1);        // same pid, same seat
    KASSERT(seat_register(8, 0x2000) == 2);
    KASSERT(seat_active() == 1 && seat_active_pid() == 7);
    KASSERT(seat_fb(2) == 0x2000);
    KASSERT(seat_switch(2) == 0);
    KASSERT(seat_active_pid() == 8);
    KASSERT(seat_switch(3) == -1);                 // no such client
    KASSERT(seat_release_pid(8) == 1);             // survivor takes over
    KASSERT(seat_active_pid() == 7);
    seat_reset();
}

// The blocking input_read syscall: a worker injects an event device-side and
// then reads it back through the full syscall path.
static volatile long inputread_res;
static void input_read_worker(void *a)
{
    (void)a;
    input_test_inject(0, EV_KEY, 30 /*KEY_A*/, 1);
    struct input_event ev;
    struct trapframe tf;
    tf.x[8] = SYS_INPUT_READ; tf.x[0] = (uint64_t)(uintptr_t)&ev;
    do_syscall(&tf);
    inputread_res = ((long)tf.x[0] == 0 && ev.type == EV_KEY &&
                     ev.code == 30 && ev.value == 1) ? 1 : -1;
}

static void test_syscall_input_read(void)
{
    pmm_init(); kheap_init();
    input_init();
    sched_init();
    inputread_res = 0;
    thread_create(input_read_worker, 0, 1);
    for (long i = 0; i < 100000 && !inputread_res; i++) { yield(); }
    KASSERT(inputread_res == 1);
}

// Regression (found live, Phase 24): SYS_READ/SYS_WRITE dispatch on ->sock
// BEFORE ->pipe, and SYS_PIPE kmalloc's its two file structs without
// initializing ->sock. kmalloc doesn't zero, so when the heap recycles a file
// struct freed by an exited socket-using process, the new pipe end carries a
// stale socket pointer and pipe I/O is silently sent to a dead socket --
// (| (run "http" ...) (run "wc")) returned 0 bytes. Poison the heap the same
// way and assert both pipe ends come out with a clean ->sock.
static volatile int pipesock_res;
static void pipe_stale_sock_worker(void *a)
{
    (void)a;
    pipesock_res = 9;                        // stage: running
    // Two file-sized blocks, filled wholesale with garbage (as a freed socket
    // file leaves behind), then freed so SYS_PIPE's kmallocs get them back.
    struct file *d1 = kmalloc(sizeof(struct file));
    struct file *d2 = kmalloc(sizeof(struct file));
    char *p1 = (char *)d1, *p2 = (char *)d2;
    for (unsigned i = 0; i < sizeof(struct file); i++) { p1[i] = (char)0xAA; p2[i] = (char)0xAA; }
    kfree(d1); kfree(d2);
    pipesock_res = 8;                        // stage: heap poisoned

    int ufd[2] = { -1, -1 };
    struct trapframe tf;
    tf.x[8] = SYS_PIPE; tf.x[0] = (uint64_t)(uintptr_t)ufd;
    do_syscall(&tf);
    if ((long)tf.x[0] != 0) { pipesock_res = 1; return; }

    struct file **fds = sched_current_fds();
    int stale = (fds[ufd[0]]->sock != 0 || fds[ufd[1]]->sock != 0);
    // Repair before exiting either way: thread_exit() vfs_close()s our fds,
    // and a poisoned ->sock would send the CLOSE path into garbage too (the
    // same trust in ->sock that makes the read path bug bite). The verdict
    // was already taken above.
    fds[ufd[0]]->sock = 0;
    fds[ufd[1]]->sock = 0;
    pipesock_res = stale ? 2 : 3;
}

static void test_pipe_file_sock_cleared(void)
{
    pmm_init(); kheap_init();
    sched_init();
    pipesock_res = 0;
    thread_create(pipe_stale_sock_worker, 0, 1);
    for (long i = 0; i < 2000000 && !(pipesock_res >= 1 && pipesock_res <= 3); i++) { yield(); }
    KASSERT(pipesock_res == 3);              // 0=never ran 9/8=hung mid-stage
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

static void test_process_groups(void)
{
    // Process groups exist for ONE reason here: Ctrl-C must reach a whole
    // job. The frame's stream-thunk forks a Lisp wrapper which forks the
    // real program (ping) -- killing just the wrapper leaves the grandchild
    // streaming forever. So: every fresh thread is its own group, setpgid
    // re-files it, and kill of a NEGATIVE pid signals every member.
    pmm_init(); kheap_init(); vm_init(); sched_init();
    struct thread *a = thread_create(sig_noop, 0, 1);
    struct thread *b = thread_create(sig_noop, 0, 1);
    struct thread *c = thread_create(sig_noop, 0, 1);
    KASSERT(a->pgid == a->id);                       // born a group of one
    KASSERT(sched_setpgid(b->id, a->id) == 0);       // b joins a's group
    KASSERT(sched_kill(-a->id, SIGINT) == 0);        // -pid = the whole group
    KASSERT(a->sig_pending & (1ull << SIGINT));
    KASSERT(b->sig_pending & (1ull << SIGINT));
    KASSERT(!(c->sig_pending & (1ull << SIGINT)));   // bystander untouched
    KASSERT(sched_kill(-9999, SIGINT) == -1);        // no such group
    KASSERT(sched_setpgid(c->id, 0) == 0);           // pgid 0 = "own id" (POSIX)
    KASSERT(c->pgid == c->id);
    KASSERT(sched_setpgid(9999, 0) == -1);           // no such pid
}

// The real aarch64 kill/setpgid numbers (129/154) that busybox's libc emits must
// dispatch to the same scheduler logic as the legacy MyOSv2 numbers (20/44).
static void test_kill_setpgid_linux_numbers(void)
{
    pmm_init(); kheap_init(); vm_init(); sched_init();
    struct thread *a = thread_create(sig_noop, 0, 1);
    struct thread *b = thread_create(sig_noop, 0, 1);
    struct trapframe tf;
    // setpgid@154: b joins a's group.
    tf.x[8] = SYS_SETPGID_LINUX; tf.x[0] = (uint64_t)b->id; tf.x[1] = (uint64_t)a->id;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == 0);
    KASSERT(b->pgid == a->id);
    // kill@129 of the whole group reaches both.
    tf.x[8] = SYS_KILL_LINUX; tf.x[0] = (uint64_t)(-a->id); tf.x[1] = SIGTERM;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == 0);
    KASSERT(a->sig_pending & (1ull << SIGTERM));
    KASSERT(b->sig_pending & (1ull << SIGTERM));
    // kill@129 of a missing pid still fails like the legacy path.
    tf.x[8] = SYS_KILL_LINUX; tf.x[0] = 9999; tf.x[1] = SIGTERM;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == -1);
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

// rt_sigaction (syscall #134): busybox/musl uses this Linux-numbered sigaction
// to install handlers. Verify that do_syscall routes it to the same per-thread
// sig_handler/sig_tramp slots that SYS_SIGNAL uses, so signals_deliver() can
// invoke busybox's handler without any changes to the delivery path.
static void test_rt_sigaction_installs(void)
{
    pmm_init(); kheap_init(); vm_init(); sched_init();
    struct thread *t = sched_current();
    uint64_t act[8];                          // [0]=handler,[1]=flags,[2]=restorer,[3..]=mask
    for (int i = 0; i < 8; i++) { act[i] = 0; }
    act[0] = 0x8000000040ULL;                 // pretend handler address in user space
    act[2] = 0x8000000080ULL;                 // restorer (sa_restorer) == our trampoline
    struct trapframe tf;
    tf.x[8] = SYS_RT_SIGACTION; tf.x[0] = SIGINT;
    tf.x[1] = (uint64_t)(uintptr_t)act; tf.x[2] = 0; tf.x[3] = 8;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == 0);                                           // success
    KASSERT((uint64_t)(uintptr_t)t->sig_handler[SIGINT] == 0x8000000040ULL); // handler stored
    KASSERT(t->sig_tramp == 0x8000000080ULL);                               // tramp from sa_restorer
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
    // Write a HIGH scratch sector (far past the ext2 metadata + fixtures, which
    // live in low blocks): /disk is now a real ext2 image, so scribbling on
    // sector 1/2/3 would corrupt the superblock/group-descriptors and break the
    // ext2: read tests. The disk is 64 MiB == 131072 sectors; 100000 is unused.
    KASSERT(block_write(100000, w) == 0);
    KASSERT(block_read(100000, r) == 0);
    for (int i = 0; i < 512; i++) { KASSERT(r[i] == w[i]); }
}

static void test_block_two_sectors(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init();
    virtio_blk_init();
    static uint8_t a[512], b[512], ra[512], rb[512];
    for (int i = 0; i < 512; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)(255 - i); }
    // High scratch sectors, away from the ext2 image's live metadata/fixtures.
    KASSERT(block_write(100002, a) == 0);
    KASSERT(block_write(100003, b) == 0);
    KASSERT(block_read(100002, ra) == 0);
    KASSERT(block_read(100003, rb) == 0);
    for (int i = 0; i < 512; i++) { KASSERT(ra[i] == a[i] && rb[i] == b[i]); }
}

// --- on-disk filesystem: ext2 READ (Phase 1) ---
//
// These run against the host-built ext2 image baked into build/disk.img by the
// Makefile (`mke2fs -b 1024 -d build/rootfs`), with deterministic fixtures:
//   /test/small.txt -- exactly "ext2-small-file-ok\n"
//   /test/big.bin    -- 16384 bytes of `yes 0123456789ABCDEF` (a 17-byte
//                       repeating pattern "0123456789ABCDEF\n"), which spans well
//                       past the 12 direct blocks (12*1024 = 12288), so reading
//                       offset 13000 exercises a single-indirect block.

static void test_ext2_mount_root_is_dir(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    struct vnode *r = ext2_mount();
    KASSERT(r != 0);
    KASSERT(r->type == VN_DIR);                      // inode 2 is the root dir
}

static void test_ext2_read_small_file(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());
    struct vnode *f = vfs_lookup("/disk/test/small.txt");
    KASSERT(f != 0 && f->type == VN_FILE);
    const char *want = "ext2-small-file-ok\n";
    int wlen = 0; while (want[wlen]) { wlen++; }     // 19 bytes
    KASSERT((int)f->size == wlen);
    char b[32] = {0};
    struct file fr = { .vnode = f, .off = 0 };
    KASSERT(vfs_read(&fr, b, sizeof(b)) == wlen);
    for (int i = 0; i < wlen; i++) { KASSERT(b[i] == want[i]); }
}

static void test_ext2_read_indirect(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());
    struct vnode *f = vfs_lookup("/disk/test/big.bin");
    KASSERT(f != 0 && f->type == VN_FILE);
    KASSERT(f->size == 16384);                       // spans past the 12 direct blocks

    // Read a single byte at offset 13000 (file block 12 -> single indirect). The
    // file is `yes 0123456789ABCDEF` = the 17-byte pattern "0123456789ABCDEF\n"
    // repeated, so byte N == pattern[N % 17].
    const char *pat = "0123456789ABCDEF\n";
    char b = 0;
    struct file fr = { .vnode = f, .off = 13000 };
    KASSERT(vfs_read(&fr, &b, 1) == 1);
    KASSERT(b == pat[13000 % 17]);                   // 13000 % 17 == 12 -> 'C'
}

static void test_ext2_readdir_test_dir(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());
    struct vnode *d = vfs_lookup("/disk/test");
    KASSERT(d != 0 && d->type == VN_DIR);
    char n[32]; int seen = 0;
    for (int i = 0; vfs_readdir(d, i, n) == 0; i++) {
        if (n[0] == 's' && n[5] == '.' && n[6] == 't') { seen |= 1; }   // small.txt
        if (n[0] == 'b' && n[3] == '.' && n[4] == 'b') { seen |= 2; }   // big.bin
    }
    KASSERT(seen == 3);                              // both fixtures present
}

static void test_vfs_mount_at(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());
    struct vnode *d = vfs_lookup("/disk");
    KASSERT(d && d->type == VN_DIR);                 // the mounted ext2 root
    KASSERT(vfs_lookup("/disk/test/small.txt") != 0);// a baked file is found
    KASSERT(vfs_lookup("/disk/nope") == 0);          // a missing one is not
}

// Symlink following: /disk/bin/ls is a symlink pointing to "busybox" (relative).
// vfs_lookup must follow it and return a vnode for the busybox binary itself --
// not a VN_SYMLINK vnode. We verify:
//   1. lookup via the symlink succeeds (not NULL).
//   2. the result's type is VN_FILE (the symlink was followed, not returned as-is).
//   3. the resolved vnode has the same size as the busybox vnode (same underlying file).
static void test_ext2_symlink_follows(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());
    struct vnode *via  = vfs_lookup("/disk/bin/ls");        // symlink -> busybox
    struct vnode *real = vfs_lookup("/disk/bin/busybox");
    KASSERT(via != 0);
    KASSERT(real != 0);
    KASSERT(via->type == VN_FILE);                          // followed to the regular file
    KASSERT(via->size == real->size);                       // same underlying file
}

// --- on-disk filesystem: ext2 WRITE (Phase 2) ---
//
// These create/write/truncate/unlink against the booted ext2 image. The
// allocator hands out low free blocks, well away from the high scratch sectors
// the `block:` tests scribble on, so the two never collide.

static void test_ext2_create_write_read(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());

    struct vnode *vn = vfs_create("/disk/t", VN_FILE);
    KASSERT(vn != 0 && vn->type == VN_FILE);
    const char *msg = "hello ext2";
    int mlen = 0; while (msg[mlen]) { mlen++; }       // 10 bytes
    struct file fw = { .vnode = vn, .off = 0 };
    KASSERT(vfs_write(&fw, msg, mlen) == mlen);

    // Re-look-up so we read back through a fresh inode read (size from disk).
    struct vnode *r = vfs_lookup("/disk/t");
    KASSERT(r != 0 && (int)r->size == mlen);
    char b[32] = {0};
    struct file fr = { .vnode = r, .off = 0 };
    KASSERT(vfs_read(&fr, b, sizeof(b)) == mlen);
    for (int i = 0; i < mlen; i++) { KASSERT(b[i] == msg[i]); }
}

static void test_ext2_write_grows_indirect(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());

    struct vnode *vn = vfs_create("/disk/big", VN_FILE);
    KASSERT(vn != 0);
    // Write 20000 bytes of a position-dependent pattern, spanning past the 12
    // direct blocks (12*1024 = 12288) so bmap_alloc must allocate a single-
    // indirect block + the data block beyond it.
    enum { N = 20000 };
    static uint8_t w[N];
    for (int i = 0; i < N; i++) { w[i] = (uint8_t)(i * 31 + 7); }
    struct file fw = { .vnode = vn, .off = 0 };
    KASSERT(vfs_write(&fw, w, N) == N);

    struct vnode *r = vfs_lookup("/disk/big");
    KASSERT(r != 0 && (int)r->size == N);
    // Verify a byte past offset 12*1024 (file block 12 -> single indirect).
    static uint8_t rb[N];
    struct file fr = { .vnode = r, .off = 0 };
    KASSERT(vfs_read(&fr, rb, N) == N);
    KASSERT(rb[13000] == (uint8_t)(13000 * 31 + 7));
    KASSERT(rb[N - 1] == (uint8_t)((N - 1) * 31 + 7));
}

static void test_ext2_truncate_resets(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());

    struct vnode *vn = vfs_create("/disk/tr", VN_FILE);
    KASSERT(vn != 0);
    struct file fw = { .vnode = vn, .off = 0 };
    KASSERT(vfs_write(&fw, "0123456789ABCDEF", 16) == 16);   // 16 bytes

    KASSERT(vfs_truncate(vn) == 0);
    KASSERT(vn->size == 0);
    struct file fw2 = { .vnode = vn, .off = 0 };
    KASSERT(vfs_write(&fw2, "xyz", 3) == 3);

    struct vnode *r = vfs_lookup("/disk/tr");
    KASSERT(r != 0 && (int)r->size == 3);
    char b[16] = {0};
    struct file fr = { .vnode = r, .off = 0 };
    KASSERT(vfs_read(&fr, b, sizeof(b)) == 3);
    KASSERT(b[0] == 'x' && b[1] == 'y' && b[2] == 'z');
}

static void test_ext2_unlink(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());

    struct vnode *vn = vfs_create("/disk/u", VN_FILE);
    KASSERT(vn != 0);
    struct file fw = { .vnode = vn, .off = 0 };
    KASSERT(vfs_write(&fw, "bye", 3) == 3);
    KASSERT(vfs_lookup("/disk/u") != 0);

    KASSERT(vfs_unlink("/disk/u") == 0);
    KASSERT(vfs_lookup("/disk/u") == 0);              // gone
}

// ext2_symlink: create a fast symlink whose target is an existing file, then
// resolve THROUGH it (vfs_lookup follows symlinks) and confirm we land on the
// target's contents. Proves the inode is written as a link and the target is
// stored/read back correctly.
static void test_ext2_symlink_create(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());

    struct vnode *t = vfs_create("/disk/sltarget", VN_FILE);
    KASSERT(t != 0);
    struct file fw = { .vnode = t, .off = 0 };
    KASSERT(vfs_write(&fw, "linked!", 7) == 7);

    KASSERT(vfs_symlink("/disk/sllink", "sltarget") == 0);   // relative target
    struct vnode *via = vfs_lookup("/disk/sllink");          // follows the link
    KASSERT(via != 0);
    KASSERT(via->type == VN_FILE && (int)via->size == 7);    // landed on the target
    char b[8] = {0};
    struct file fr = { .vnode = via, .off = 0 };
    KASSERT(vfs_read(&fr, b, 7) == 7 && bytes_eq(b, "linked!", 7));
}

// ext2_rename: move a file to a new name; the old name disappears and the new
// name carries the SAME contents (the inode is relinked, not copied).
static void test_ext2_rename_moves(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());

    struct vnode *vn = vfs_create("/disk/rnA", VN_FILE);
    KASSERT(vn != 0);
    struct file fw = { .vnode = vn, .off = 0 };
    KASSERT(vfs_write(&fw, "movable", 7) == 7);

    KASSERT(vfs_rename("/disk/rnA", "/disk/rnB") == 0);
    KASSERT(vfs_lookup("/disk/rnA") == 0);                   // old name gone
    struct vnode *r = vfs_lookup("/disk/rnB");
    KASSERT(r != 0 && (int)r->size == 7);                    // new name, same data
    char b[8] = {0};
    struct file fr = { .vnode = r, .off = 0 };
    KASSERT(vfs_read(&fr, b, 7) == 7 && bytes_eq(b, "movable", 7));
}

// ext2_link: a hard link gives one inode a second name. Both names read the
// same data, and removing one leaves the other intact (link count > 0).
static void test_ext2_hardlink(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());

    struct vnode *vn = vfs_create("/disk/hlA", VN_FILE);
    KASSERT(vn != 0);
    struct file fw = { .vnode = vn, .off = 0 };
    KASSERT(vfs_write(&fw, "shared", 6) == 6);

    KASSERT(vfs_link("/disk/hlA", "/disk/hlB") == 0);
    struct vnode *b = vfs_lookup("/disk/hlB");
    KASSERT(b != 0 && (int)b->size == 6);                 // second name, same data
    char buf[8] = {0};
    struct file fr = { .vnode = b, .off = 0 };
    KASSERT(vfs_read(&fr, buf, 6) == 6 && bytes_eq(buf, "shared", 6));

    // Removing one name keeps the data reachable through the other.
    KASSERT(vfs_unlink("/disk/hlA") == 0);
    KASSERT(vfs_lookup("/disk/hlA") == 0);
    struct vnode *still = vfs_lookup("/disk/hlB");
    KASSERT(still != 0 && (int)still->size == 6);
}

static void test_ext2_persists_remount(void)
{
    if (!DISK_TESTS) { return; }
    pmm_init(); kheap_init(); virtio_blk_init();
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());

    struct vnode *vn = vfs_create("/disk/p", VN_FILE);
    KASSERT(vn != 0);
    const char *msg = "persist me!";
    int mlen = 0; while (msg[mlen]) { mlen++; }
    struct file fw = { .vnode = vn, .off = 0 };
    KASSERT(vfs_write(&fw, msg, mlen) == mlen);

    // Drop the vnodes and mount fresh: this re-reads the superblock, group
    // descriptors, bitmaps and inode from disk. If create/write only mutated the
    // RAM cache, the file would be gone (or empty) here.
    vfs_mount_root(ramfs_type());
    vfs_mount_at("/disk", ext2_mount());

    struct vnode *r = vfs_lookup("/disk/p");
    KASSERT(r != 0 && (int)r->size == mlen);
    char b[32] = {0};
    struct file fr = { .vnode = r, .off = 0 };
    KASSERT(vfs_read(&fr, b, sizeof(b)) == mlen);
    for (int i = 0; i < mlen; i++) { KASSERT(b[i] == msg[i]); }
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

static void test_resolve_dotted_quad(void)
{
    // The dotted-quad fast path returns before any net I/O, so this needs no
    // virtio_net_init -- it is pure parsing of the host string.
    uint32_t ip = 0;
    KASSERT(net_resolve("10.0.2.2", &ip) == 0);
    KASSERT(ip == IP_GATEWAY);                                // 0x0a000202
    ip = 0;
    KASSERT(net_resolve("1.2.3.4", &ip) == 0);
    KASSERT(ip == 0x01020304u);
    // (A non-quad host falls through to DNS, which needs an initialised stack;
    //  that path is covered by the live DNS tests, not here.)
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

// --- UDP sockets (Phase 22) ---
static void test_socket_udp_queue(void)
{
    pmm_init(); kheap_init();
    struct socket *s = socket_alloc(SOCK_DGRAM);
    KASSERT(s != 0);
    KASSERT(socket_bind(s, 1234) == 0);

    // A datagram delivered to our port is queued and returned by recvfrom (which
    // doesn't block because data is already present).
    uint8_t in[3] = { 'h', 'i', '!' };
    socket_udp_input(0x0a000202u, 5555, 1234, in, 3);
    socket_udp_input(0x01020304u, 7, 9999, in, 3);   // other port -> not for us

    uint8_t out[8]; uint32_t sip = 0; uint16_t sport = 0;
    int n = socket_recvfrom(s, out, sizeof(out), &sip, &sport);
    KASSERT(n == 3);
    KASSERT(out[0] == 'h' && out[1] == 'i' && out[2] == '!');
    KASSERT(sip == 0x0a000202u && sport == 5555);
    socket_free(s);
}

static void test_tcp_checksum(void)
{
    // A header with the checksum field set to its computed value must verify to 0
    // (the standard internet-checksum property, including the TCP pseudo-header).
    uint8_t seg[20] = {0};
    seg[0] = 0x12; seg[1] = 0x34;      // source port
    seg[2] = 0x00; seg[3] = 0x50;      // dest port 80
    seg[12] = 5 << 4;                  // data offset
    seg[13] = 0x02;                    // SYN
    uint32_t sip = 0x0a00020fu, dip = 0x08080808u;
    uint16_t c = tcp_checksum(sip, dip, seg, 20);
    seg[16] = (uint8_t)(c >> 8); seg[17] = (uint8_t)c;
    KASSERT(tcp_checksum(sip, dip, seg, 20) == 0);
}

// --- TCP reassembly queue (Phase 23.1) ---
// These tests use a file-scope instance because struct tcp_reasm is ~9 KiB --
// too large for the boot/test stack the self-tests run on.
static struct tcp_reasm g_reasm;

static void test_tcp_reasm_in_order(void)
{
    tcp_reasm_init(&g_reasm, 1000);
    tcp_reasm_accept(&g_reasm, 1000, (const uint8_t *)"abc", 3);
    uint8_t out[8] = {0};
    int n = tcp_reasm_read(&g_reasm, out, sizeof(out));
    KASSERT(n == 3);
    KASSERT(out[0] == 'a' && out[1] == 'b' && out[2] == 'c');
    KASSERT(tcp_reasm_pos(&g_reasm) == 1003);     // advanced past the run
}

static void test_tcp_reasm_out_of_order(void)
{
    tcp_reasm_init(&g_reasm, 1000);
    uint8_t out[16] = {0};

    // A segment that arrives AHEAD of the gap is held, not delivered yet.
    tcp_reasm_accept(&g_reasm, 1003, (const uint8_t *)"def", 3);
    KASSERT(tcp_reasm_read(&g_reasm, out, sizeof(out)) == 0);
    KASSERT(tcp_reasm_pos(&g_reasm) == 1000);     // still waiting on [1000,1003)

    // Filling the gap releases BOTH segments as one contiguous run.
    tcp_reasm_accept(&g_reasm, 1000, (const uint8_t *)"abc", 3);
    int n = tcp_reasm_read(&g_reasm, out, sizeof(out));
    KASSERT(n == 6);
    KASSERT(out[0]=='a' && out[1]=='b' && out[2]=='c' &&
            out[3]=='d' && out[4]=='e' && out[5]=='f');
    KASSERT(tcp_reasm_pos(&g_reasm) == 1006);
}

static void test_tcp_reasm_wraps(void)
{
    // base near 2^32: a run that straddles the 32-bit sequence wrap must still
    // reassemble in order (the comparisons use signed differences).
    uint32_t base = 0xfffffffeu;                  // -2
    tcp_reasm_init(&g_reasm, base);
    uint8_t out[8] = {0};
    tcp_reasm_accept(&g_reasm, base + 1, (const uint8_t *)"YZ", 2);  // ahead of gap
    KASSERT(tcp_reasm_read(&g_reasm, out, sizeof(out)) == 0);
    tcp_reasm_accept(&g_reasm, base, (const uint8_t *)"X", 1);       // fills gap
    int n = tcp_reasm_read(&g_reasm, out, sizeof(out));
    KASSERT(n == 3);
    KASSERT(out[0]=='X' && out[1]=='Y' && out[2]=='Z');
    KASSERT(tcp_reasm_pos(&g_reasm) == base + 3);  // 0xfffffffe + 3 = 1 (wrapped)
}

static void test_tcp_reasm_dup_and_out_of_window(void)
{
    tcp_reasm_init(&g_reasm, 1000);
    uint8_t out[16] = {0};

    // Overlapping retransmits are idempotent: "abc"@1000 then "bcd"@1001 -> abcd.
    tcp_reasm_accept(&g_reasm, 1000, (const uint8_t *)"abc", 3);
    tcp_reasm_accept(&g_reasm, 1001, (const uint8_t *)"bcd", 3);
    int n = tcp_reasm_read(&g_reasm, out, sizeof(out));
    KASSERT(n == 4 && out[0]=='a' && out[1]=='b' && out[2]=='c' && out[3]=='d');
    KASSERT(tcp_reasm_pos(&g_reasm) == 1004);

    // Already-consumed bytes (before base) are ignored, not re-delivered.
    tcp_reasm_accept(&g_reasm, 1000, (const uint8_t *)"abc", 3);
    KASSERT(tcp_reasm_read(&g_reasm, out, sizeof(out)) == 0);
    KASSERT(tcp_reasm_pos(&g_reasm) == 1004);

    // Bytes at/after base+WIN are too far ahead to buffer -> dropped, leaving a
    // gap so nothing in front of them is delivered.
    tcp_reasm_accept(&g_reasm, 1004 + TCP_REASM_WIN, (const uint8_t *)"z", 1);
    KASSERT(tcp_reasm_read(&g_reasm, out, sizeof(out)) == 0);
}

// --- TCP RTO estimator (Phase 23.2, RFC 6298) ---
static void test_tcp_rto_first_sample(void)
{
    struct tcp_rto e;
    tcp_rto_init(&e);
    KASSERT(tcp_rto_get(&e) == TCP_RTO_INIT);     // 1 s before any measurement

    // First sample R: SRTT = R, RTTVAR = R/2, RTO = SRTT + 4*RTTVAR = 3R.
    tcp_rto_sample(&e, 100000);                   // 100 ms
    KASSERT(tcp_rto_get(&e) == 300000);           // 100ms + 4*50ms = 300 ms
}

static void test_tcp_rto_backoff_and_clamp(void)
{
    struct tcp_rto e;
    tcp_rto_init(&e);
    tcp_rto_sample(&e, 100000);                   // RTO = 300 ms
    tcp_rto_backoff(&e);
    KASSERT(tcp_rto_get(&e) == 600000);           // doubled
    tcp_rto_backoff(&e);
    KASSERT(tcp_rto_get(&e) == 1200000);          // doubled again
    for (int i = 0; i < 40; i++) { tcp_rto_backoff(&e); }
    KASSERT(tcp_rto_get(&e) == TCP_RTO_MAX);       // capped, never overflows

    // A clean sample collapses the backoff back to the measured estimate.
    tcp_rto_sample(&e, 100000);
    KASSERT(tcp_rto_get(&e) < 1000000);

    // A tiny RTT is floored at TCP_RTO_MIN, never zero.
    struct tcp_rto f;
    tcp_rto_init(&f);
    tcp_rto_sample(&f, 1000);                      // 1 ms -> raw RTO 3 ms
    KASSERT(tcp_rto_get(&f) == TCP_RTO_MIN);       // clamped up to the floor
}

// --- TCP flow-control window arithmetic (Phase 23.3) ---
static void test_tcp_flow_windows(void)
{
    // Advertised window = free space, capped to the reassembly buffer and the
    // 16-bit field; never negative.
    KASSERT(tcp_advertise_wnd(1000) == 1000);
    KASSERT(tcp_advertise_wnd(0) == 0);
    KASSERT(tcp_advertise_wnd(-5) == 0);
    KASSERT(tcp_advertise_wnd(100000) == TCP_REASM_WIN);   // capped to buffer

    // Sendable bytes = room left in the peer's window beyond what's in flight,
    // capped at the MSS.
    KASSERT(tcp_window_avail(100, 100, 500, 1400) == 500);  // nothing in flight
    KASSERT(tcp_window_avail(100, 600, 500, 1400) == 0);    // window full
    KASSERT(tcp_window_avail(100, 100, 5000, 1400) == 1400);// MSS-capped
    KASSERT(tcp_window_avail(100, 300, 500, 1400) == 300);  // 200 in flight, 300 left
}

// --- UDP transmit checksum (Phase 23.9) ---
static void test_udp_checksum(void)
{
    // A datagram carrying its own computed checksum verifies to all-ones (0xffff),
    // i.e. the raw one's-complement sum is zero -- the standard checksum property.
    uint8_t seg[12] = {0};
    seg[0] = 0x30; seg[1] = 0x39;          // source port 12345
    seg[2] = 0x00; seg[3] = 0x35;          // dest port 53
    seg[4] = 0x00; seg[5] = 0x0c;          // UDP length = 12
    seg[8] = 'h'; seg[9] = 'i';            // 4 bytes of payload (10,11 = 0)
    uint32_t sip = 0x0a00020fu, dip = 0x0a000203u;
    uint16_t c = udp_checksum(sip, dip, seg, 12);
    KASSERT(c != 0);                        // a real checksum, never the "none" value
    seg[6] = (uint8_t)(c >> 8); seg[7] = (uint8_t)c;
    KASSERT(udp_checksum(sip, dip, seg, 12) == 0xffff);
}

// --- DHCP reply parsing (Phase 23.9) ---
static void test_dhcp_parse(void)
{
    uint8_t msg[300];
    for (unsigned i = 0; i < sizeof(msg); i++) { msg[i] = 0; }
    msg[0] = 2;                                  // op = BOOTREPLY
    msg[4] = 0xde; msg[5] = 0xad; msg[6] = 0xbe; msg[7] = 0xef;       // xid
    msg[16] = 0x0a; msg[17] = 0x00; msg[18] = 0x02; msg[19] = 0x0f;   // yiaddr 10.0.2.15
    msg[236] = 0x63; msg[237] = 0x82; msg[238] = 0x53; msg[239] = 0x63; // magic cookie
    int o = 240;
    msg[o++] = 53; msg[o++] = 1; msg[o++] = 5;                        // type = ACK
    msg[o++] = 54; msg[o++] = 4;
    msg[o++] = 0x0a; msg[o++] = 0x00; msg[o++] = 0x02; msg[o++] = 0x02; // server 10.0.2.2
    msg[o++] = 1;  msg[o++] = 4;
    msg[o++] = 0xff; msg[o++] = 0xff; msg[o++] = 0xff; msg[o++] = 0x00; // mask /24
    msg[o++] = 3;  msg[o++] = 4;
    msg[o++] = 0x0a; msg[o++] = 0x00; msg[o++] = 0x02; msg[o++] = 0x02; // router 10.0.2.2
    msg[o++] = 6;  msg[o++] = 4;
    msg[o++] = 0x0a; msg[o++] = 0x00; msg[o++] = 0x02; msg[o++] = 0x03; // DNS 10.0.2.3
    msg[o++] = 51; msg[o++] = 4;
    msg[o++] = 0x00; msg[o++] = 0x00; msg[o++] = 0x0e; msg[o++] = 0x10; // lease 3600 s
    msg[o++] = 255;

    struct dhcp_lease L;
    KASSERT(dhcp_parse(msg, o, 0xdeadbeefu, &L) == 0);
    KASSERT(L.type == 5 && L.yiaddr == 0x0a00020fu && L.server == 0x0a000202u);
    KASSERT(L.mask == 0xffffff00u && L.router == 0x0a000202u && L.dns == 0x0a000203u);
    KASSERT(L.lease_secs == 3600);
    KASSERT(dhcp_parse(msg, o, 0x12345678u, &L) != 0);    // wrong xid -> reject
}

static void test_dhcp_lease_action(void)
{
    // T1 = half the lease, T2 = 7/8, expiry = full (microseconds).
    uint64_t t1 = 1800000000ull, t2 = 3150000000ull, exp = 3600000000ull;
    KASSERT(dhcp_lease_action(1000000000ull, t1, t2, exp) == DHCP_HOLD);       // before T1
    KASSERT(dhcp_lease_action(2000000000ull, t1, t2, exp) == DHCP_RENEW);      // T1..T2
    KASSERT(dhcp_lease_action(3200000000ull, t1, t2, exp) == DHCP_REBIND);     // T2..expiry
    KASSERT(dhcp_lease_action(3700000000ull, t1, t2, exp) == DHCP_REACQUIRE);  // expired
}

// --- TCP RST generation (Phase 23.7) ---
static void test_tcp_rst_fields(void)
{
    enum { F_SYN = 0x02, F_RST = 0x04, F_ACK = 0x10 };
    uint32_t seq, ack; unsigned char fl;

    // An offending segment that carried an ACK: the RST takes its seq from that
    // ack and carries no ACK of its own.
    tcp_rst_fields(F_ACK, 5000, 9000, 0, &seq, &ack, &fl);
    KASSERT(seq == 9000 && fl == F_RST);

    // A bare SYN (no ACK), one sequence number of "data": RST+ACK acknowledging
    // seq+1 so the peer accepts the reset.
    tcp_rst_fields(F_SYN, 5000, 0, 1, &seq, &ack, &fl);
    KASSERT(seq == 0 && ack == 5001 && fl == (F_RST | F_ACK));
}

// --- TCP congestion control (Phase 23.6, Reno) ---
static void test_tcp_cc_slow_start(void)
{
    struct tcp_cc cc;
    tcp_cc_init(&cc, 1000);
    KASSERT(tcp_cc_cwnd(&cc) == 4000);            // initial window = 4*mss

    // Slow start: each ACK adds one MSS (exponential growth per RTT).
    tcp_cc_on_ack(&cc, 1000);
    KASSERT(tcp_cc_cwnd(&cc) == 5000);
    tcp_cc_on_ack(&cc, 1000);
    KASSERT(tcp_cc_cwnd(&cc) == 6000);
}

static void test_tcp_cc_avoidance_and_loss(void)
{
    struct tcp_cc cc;
    tcp_cc_init(&cc, 1000);

    // Force congestion avoidance by lowering ssthresh below cwnd: each ACK then
    // adds only ~mss*mss/cwnd (here 1000*1000/4000 = 250), not a whole MSS.
    cc.ssthresh = 2000;
    tcp_cc_on_ack(&cc, 1000);
    KASSERT(tcp_cc_cwnd(&cc) == 4250);

    // Three duplicate ACKs -> fast retransmit signalled on the third; ssthresh
    // halves the window (4250/2 = 2125) and cwnd = ssthresh + 3*mss (recovery).
    KASSERT(tcp_cc_on_dupack(&cc, 1000) == 0);
    KASSERT(tcp_cc_on_dupack(&cc, 1000) == 0);
    KASSERT(tcp_cc_on_dupack(&cc, 1000) == 1);     // the 3rd triggers retransmit
    KASSERT(cc.ssthresh == 2125);
    KASSERT(tcp_cc_cwnd(&cc) == 2125 + 3000);

    // A new ACK ends recovery: cwnd deflates back to ssthresh.
    tcp_cc_on_ack(&cc, 1000);
    KASSERT(tcp_cc_cwnd(&cc) == 2125);

    // A timeout is severe: cwnd collapses to one MSS, ssthresh halves again.
    tcp_cc_on_timeout(&cc, 1000);
    KASSERT(tcp_cc_cwnd(&cc) == 1000);
    KASSERT(cc.ssthresh == 2000);                  // max(2125/2=1062, 2*mss=2000)
}

// --- TCP segmentation / Nagle (Phase 23.8) ---
static void test_tcp_next_seg(void)
{
    // Plenty of data, nothing in flight, big window: send a full MSS.
    KASSERT(tcp_next_seg(5000, 0, 8000, 1400) == 1400);
    // A lone small write with nothing in flight goes out immediately.
    KASSERT(tcp_next_seg(500, 0, 8000, 1400) == 500);
    // First segment into a small window (nothing in flight) uses the window.
    KASSERT(tcp_next_seg(5000, 0, 1000, 1400) == 1000);
    // Nagle: a small remainder while data is in flight waits (returns 0).
    KASSERT(tcp_next_seg(500, 1400, 8000, 1400) == 0);
    // Window-limited to a sub-MSS slice while in flight: also wait (silly-window).
    KASSERT(tcp_next_seg(5000, 7000, 8000, 1400) == 0);
    // Window full: wait.
    KASSERT(tcp_next_seg(5000, 8000, 8000, 1400) == 0);
    // Nothing to send.
    KASSERT(tcp_next_seg(0, 0, 8000, 1400) == 0);
}

// --- poll() readiness scan (Phase 23.5), exercised over pipes ---
static void test_poll_pipe_readiness(void)
{
    pmm_init(); kheap_init();
    struct pipe *p = pipe_alloc();                 // readers = writers = 1
    struct file rd = { .pipe = p, .writable = 0, .ref = 1 };
    struct file wr = { .pipe = p, .writable = 1, .ref = 1 };
    struct file *fds[16];
    for (int i = 0; i < 16; i++) { fds[i] = 0; }
    fds[3] = &rd; fds[4] = &wr;

    struct pollfd pf[2] = {
        { .fd = 3, .events = POLLIN,  .revents = 0 },
        { .fd = 4, .events = POLLOUT, .revents = 0 },
    };

    // Empty pipe: nothing to read, but room to write.
    KASSERT(poll_scan(fds, pf, 2, 16) == 1);
    KASSERT(pf[0].revents == 0);
    KASSERT(pf[1].revents == POLLOUT);

    // After a write, the read end is readable too.
    pipe_write(&wr, "hi", 2);
    KASSERT(poll_scan(fds, pf, 2, 16) == 2);
    KASSERT(pf[0].revents == POLLIN);
    KASSERT(pf[1].revents == POLLOUT);

    // A descriptor with no open file reports POLLERR.
    struct pollfd bad = { .fd = 9, .events = POLLIN, .revents = 0 };
    KASSERT(poll_scan(fds, &bad, 1, 16) == 1);
    KASSERT(bad.revents == POLLERR);
}

// --- Lisp machine (lm_core: reader, evaluator, printer, GC) ---
//
// The portable Lisp core is compiled into the kernel precisely so we can
// red-green it here, on-target, under `make test`. Each test boots a fresh Lisp
// image (lm_boot resets the heap + obarray), evaluates source from a string, and
// asserts on the printed result -- the same read-eval-print path the real REPL
// uses, minus the tty.

static int lm_streq_(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Re-init the allocators (lm_alloc -> kmalloc needs a live heap) then boot a
// fresh Lisp image. The Lisp tests run before the heap tests in the registry,
// so -- like every other test here -- they must stand up their own allocators.
static void lm_fresh(void)
{
    pmm_init();
    kheap_init();
    lm_boot();
}

// Evaluate all forms in `src`, print the last result, compare to `expect`.
static int lm_is(const char *src, const char *expect)
{
    char buf[256];
    lm_print_cstr(lm_eval_all_str(src), buf, sizeof(buf));
    return lm_streq_(buf, expect);
}

static void test_lm_arithmetic(void)
{
    lm_fresh();
    KASSERT(lm_is("(+ 1 2 3)", "6"));
    KASSERT(lm_is("(* (+ 1 2) (- 10 6))", "12"));
    KASSERT(lm_is("(- 5)", "-5"));
    KASSERT(lm_is("(/ 20 4)", "5"));
    KASSERT(lm_is("(% 17 5)", "2"));
}

static void test_lm_lists(void)
{
    lm_fresh();
    KASSERT(lm_is("(quote (1 2 3))", "(1 2 3)"));
    KASSERT(lm_is("(cons 1 2)", "(1 . 2)"));
    KASSERT(lm_is("(car (quote (a b c)))", "a"));
    KASSERT(lm_is("(cdr (quote (a b c)))", "(b c)"));
    KASSERT(lm_is("(list 1 2 (+ 1 2))", "(1 2 3)"));
}

static void test_lm_conditionals(void)
{
    lm_fresh();
    KASSERT(lm_is("(if (< 1 2) 10 20)", "10"));
    KASSERT(lm_is("(if (> 1 2) 10 20)", "20"));
    KASSERT(lm_is("(cond ((= 1 2) 'a) ((= 2 2) 'b) (t 'c))", "b"));
    KASSERT(lm_is("(and 1 2 3)", "3"));
    KASSERT(lm_is("(or nil nil 7)", "7"));
    KASSERT(lm_is("(not nil)", "t"));
}

static void test_lm_let_and_setq(void)
{
    lm_fresh();
    KASSERT(lm_is("(let ((x 5) (y 7)) (+ x y))", "12"));
    KASSERT(lm_is("(progn (setq a 3) (setq a (+ a 4)) a)", "7"));
}

static void test_lm_defun_and_recursion(void)
{
    lm_fresh();
    KASSERT(lm_is("(defun sq (x) (* x x)) (sq 9)", "81"));
    // Recursion + the evaluator's tail-call path.
    KASSERT(lm_is("(defun fact (n) (if (= n 0) 1 (* n (fact (- n 1))))) (fact 5)", "120"));
}

static void test_lm_closures(void)
{
    lm_fresh();
    KASSERT(lm_is("(defun adder (n) (lambda (x) (+ x n))) (funcall (adder 10) 5)", "15"));
}

static void test_lm_macros(void)
{
    lm_fresh();
    KASSERT(lm_is("(defmacro inc (v) (list 'setq v (list '+ v 1)))"
                  " (setq a 1) (inc a) (inc a) a", "3"));
}

static void test_lm_higher_order(void)
{
    lm_fresh();
    KASSERT(lm_is("(mapcar (lambda (x) (* x x)) (quote (1 2 3 4)))", "(1 4 9 16)"));
    KASSERT(lm_is("(apply (function +) (quote (1 2 3 4)))", "10"));
}

static void test_lm_strings(void)
{
    lm_fresh();
    KASSERT(lm_is("(string-concat \"ab\" \"cd\")", "\"abcd\""));
    KASSERT(lm_is("(string-length \"hello\")", "5"));
    KASSERT(lm_is("(equal \"hi\" \"hi\")", "t"));
    KASSERT(lm_is("(type-of 5)", "fixnum"));
    KASSERT(lm_is("(type-of 'x)", "symbol"));
}

static void test_lm_gc_keeps_roots(void)
{
    lm_fresh();
    lm_eval_all_str("(setq keep (list 1 2 3))");   // rooted via the obarray
    lm_eval_all_str("(progn (cons 1 2) (cons 3 4) nil)");  // pure garbage
    size_t before = gc.alloc_count;
    gc_collect(global_env);
    size_t after = gc.alloc_count;
    KASSERT(after < before);                       // garbage was reclaimed
    KASSERT(lm_is("keep", "(1 2 3)"));             // the rooted list survived
    // The image is still fully functional after a collection.
    KASSERT(lm_is("(+ 2 2)", "4"));
}

static void test_lm_error_recovery(void)
{
    lm_fresh();
    // An unbound variable raises an error that unwinds to the recovery point;
    // the image keeps working afterwards (a typo at the REPL must not kill init).
    Lobj r = lm_eval_cstr("nosuchvariable");
    KASSERT(r == Qnil);
    KASSERT(lm_is("(+ 40 2)", "42"));
}

// Does `hay` contain `needle`? (No strstr in a freestanding kernel.)
static int lm_strhas_(const char *hay, const char *needle)
{
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) { return 1; }
    }
    return !*needle;
}

static void test_lm_rest_params(void)
{
    lm_fresh();
    // A bare symbol in place of the parameter list binds ALL arguments to it
    // (the classic &rest). The Lisp shell's (run "cmd" "arg" ...) needs this:
    // a command takes any number of arguments.
    KASSERT(lm_is("(defun f args args) (f 1 2 3)", "(1 2 3)"));
    KASSERT(lm_is("(funcall (lambda args (car args)) 7 8)", "7"));
    // No arguments -> the rest parameter is nil, not unbound.
    KASSERT(lm_is("(defun g args args) (g)", "nil"));
    // `|` must read as an ordinary symbol so the pipeline macro can bear the
    // shell's traditional name.
    KASSERT(lm_is("(setq | 5) |", "5"));
}

static void test_lm_global_nil_binding(void)
{
    lm_fresh();
    // A global set to nil is BOUND-to-nil, not unbound. Found live: frame.l's
    // (setq pending-cx nil) made every later read of pending-cx an unbound-
    // variable error, because symbol value slots initialized to Qnil and the
    // lookup used Qnil to mean "no binding". An explicit unbound sentinel
    // separates the two.
    KASSERT(lm_is("(setq flag nil) (if flag 1 2)", "2"));
    KASSERT(lm_is("(setq flag2 nil) flag2", "nil"));
    // Genuinely unbound still errors (and recovers).
    Lobj r = lm_eval_cstr("definitely-not-bound");
    KASSERT(r == Qnil);
}

static void test_lm_eval_primitive(void)
{
    lm_fresh();
    // (eval form) -- the missing third of read/eval/print, so a REPL can be
    // written IN Lisp (system.l builds the shell's repl from these).
    KASSERT(lm_is("(eval (list '+ 1 2))", "3"));
    KASSERT(lm_is("(eval ''x)", "x"));
}

static void test_lm_error_goes_to_cur_out(void)
{
    lm_fresh();
    // The REPL may be a TCP socket (24.1b): a remote Emacs user must see error
    // messages in their buffer, not on the guest's serial console. So lm_error
    // must write through lm_cur_out when it is set (and only fall back to the
    // raw fd 2 when it isn't, e.g. during load before the REPL starts).
    char buf[128];
    Writer w;
    writer_to_buffer(&w, buf, sizeof(buf));
    lm_cur_out = &w;
    lm_eval_cstr("nosuchvariable");      // raises + recovers
    lm_cur_out = 0;
    KASSERT(lm_strhas_(buf, "ERROR"));
    KASSERT(lm_strhas_(buf, "nosuchvariable"));
}

// --- Busybox/musl syscalls: getppid, fcntl, clock_gettime (Phase BB2) ---
//
// These three KTESTs are written BEFORE the handler implementations so they go
// red first (all return -ENOSYS) and then green once the cases are added to
// do_syscall(). That is the test-first discipline we follow.
//
// Trapframe init idiom: field-by-field assignment, identical to test_ioctl_tcgets_is_tty
// above. We never use `struct trapframe tf = {0}` in this freestanding kernel because
// GCC lowers that to a __aeabi_memset / memset call that cannot link without libc.

static void test_getppid_returns_parent(void)
{
    // SYS_GETPPID (173) must return >= 0. In the KTEST harness sched_current()
    // may be the idle/boot thread (id 0) with no parent; in that case the
    // handler returns 1 (init pid) as a safe sentinel. Either way >= 0 is correct.
    struct trapframe tf;
    tf.x[8] = SYS_GETPPID;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] >= 0);
}

static void test_fcntl_getfl_rdwr(void)
{
    // F_GETFL (3) on any open fd must return the open-mode flags.  We have no
    // per-fd flags table yet, so we return O_RDWR (2) for everything -- the
    // minimal answer that lets busybox probe a file-descriptor and carry on.
    struct trapframe tf;
    tf.x[8] = SYS_FCNTL;
    tf.x[0] = 0;          // fd 0 (stdin)
    tf.x[1] = F_GETFL;    // get file-status flags
    tf.x[2] = 0;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == 2);   // O_RDWR
}

static void test_clock_gettime_ok(void)
{
    // SYS_CLOCK_GETTIME (113): x0=clockid (ignored), x1=struct timespec*.
    // We accept any clock id and fill tv_sec from timer_ticks()/1000,
    // tv_nsec from the remainder. The return value must be 0 (success).
    long ts[2];
    ts[0] = 0; ts[1] = 0;
    struct trapframe tf;
    tf.x[8] = SYS_CLOCK_GETTIME;
    tf.x[0] = 1;                              // CLOCK_MONOTONIC (ignored by us)
    tf.x[1] = (uint64_t)(uintptr_t)ts;        // -> struct timespec {tv_sec, tv_nsec}
    tf.x[2] = 0;
    do_syscall(&tf);
    KASSERT((long)tf.x[0] == 0);             // must succeed
}

// dup3(oldfd, newfd, flags): the real aarch64 number musl's dup2()/dup3() emit
// (the kernel previously only had the legacy MyOSv2 dup2 #19, which musl never
// calls). A shell uses it for redirection: `>file` opens the file then dups it
// onto fd 1. We duplicate oldfd into the EXACT slot newfd (closing whatever was
// there first), and -- unlike dup2 -- reject oldfd==newfd with EINVAL.
static int dup3_ok;
static void dup3_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    int ok = 1;
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/hello.txt"; tf.x[2] = 0; tf.x[3] = 0;
    do_syscall(&tf);
    long fd = (long)tf.x[0];
    ok = ok && (fd >= 3);
    // Duplicate onto a specific high slot; dup3 must return that exact fd.
    tf.x[8] = SYS_DUP3; tf.x[0] = (uint64_t)fd; tf.x[1] = 7; tf.x[2] = 0;
    do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 7);
    struct file **fds = sched_current_fds();
    ok = ok && fds && fds[7] && (fds[7]->vnode == fds[fd]->vnode);   // same underlying file
    // dup3 of a fd onto itself is EINVAL (this is the one way it differs from dup2).
    tf.x[8] = SYS_DUP3; tf.x[0] = (uint64_t)fd; tf.x[1] = (uint64_t)fd; tf.x[2] = 0;
    do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == -EINVAL);
    dup3_ok = ok ? 1 : -1;
}
static void test_syscall_dup3(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    dup3_ok = 0;
    sched_init();
    thread_create(dup3_worker, 0, 1);
    while (dup3_ok == 0) { yield(); }
    KASSERT(dup3_ok == 1);
}

// pipe2(int fd[2], flags): the real aarch64 number musl's pipe() emits (aarch64
// Linux has no plain pipe). Identical to the legacy SYS_PIPE except for the
// flags arg (O_CLOEXEC/O_NONBLOCK -- ignored) and negative-errno on failure. A
// shell needs it for `a | b`. We verify it returns two distinct usable fds and
// that bytes written to the write end come back out the read end.
static int pipe2_ok;
static void pipe2_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    int ok = 1;
    int ufd[2] = { -1, -1 };
    tf.x[8] = SYS_PIPE2; tf.x[0] = (uint64_t)(uintptr_t)ufd; tf.x[1] = 0;
    do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 0);
    ok = ok && (ufd[0] >= 3) && (ufd[1] >= 3) && (ufd[0] != ufd[1]);
    // Round-trip a byte through the pipe.
    char wb = 'Z';
    tf.x[8] = SYS_WRITE; tf.x[0] = (uint64_t)ufd[1];
    tf.x[1] = (uint64_t)(uintptr_t)&wb; tf.x[2] = 1; do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 1);
    char rb = 0;
    tf.x[8] = SYS_READ; tf.x[0] = (uint64_t)ufd[0];
    tf.x[1] = (uint64_t)(uintptr_t)&rb; tf.x[2] = 1; do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 1) && (rb == 'Z');
    pipe2_ok = ok ? 1 : -1;
}
static void test_syscall_pipe2(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    pipe2_ok = 0;
    sched_init();
    thread_create(pipe2_worker, 0, 1);
    while (pipe2_ok == 0) { yield(); }
    KASSERT(pipe2_ok == 1);
}

// nanosleep(req, rem): the real number musl's sleep()/usleep() emit. We convert
// the timespec to milliseconds and route to the same sleep_ms() blocking path
// SYS_SLEEP uses, so the thread must actually de-schedule for the requested time
// and resume after. Mirrors test_syscall_sleep_blocks: req = 3 ms blocks for
// exactly 3 timer ticks. (rem is for interrupted sleeps; unused on this path.)
static char ns_log[8];
static int ns_n;
static void nanosleep_worker(void *a)
{
    (void)a;
    long req[2] = { 0, 3 * 1000000L };   // {tv_sec=0, tv_nsec=3,000,000} = 3 ms
    ns_log[ns_n++] = 'S';
    struct trapframe tf;
    tf.x[8] = SYS_NANOSLEEP;
    tf.x[0] = (uint64_t)(uintptr_t)req;
    tf.x[1] = 0;
    do_syscall(&tf);
    ns_log[ns_n++] = 'W';
}
static void test_syscall_nanosleep_blocks(void)
{
    pmm_init(); kheap_init();
    ns_n = 0;
    sched_init();
    thread_create(nanosleep_worker, 0, 1);
    yield(); KASSERT(ns_n == 1); KASSERT(ns_log[0] == 'S');   // slept, not yet woken
    sched_tick(); yield(); KASSERT(ns_n == 1);
    sched_tick(); yield(); KASSERT(ns_n == 1);
    sched_tick(); yield();
    KASSERT(ns_n == 2); KASSERT(ns_log[1] == 'W');            // woke after 3 ticks
}

// fchmodat(dirfd, path, mode, flags): busybox `chmod` emits it. We store no
// permission bits (single-user system, everything is root-owned and accessible),
// so it is an existence-checked no-op: 0 if the path resolves, -ENOENT if not --
// which is enough for `chmod` to succeed on a real file and fail on a missing
// one. (Same shape as faccessat/utimensat.)
static int fchmodat_ok;
static void fchmodat_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    int ok = 1;
    tf.x[8] = SYS_FCHMODAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/hello.txt"; tf.x[2] = 0644; tf.x[3] = 0;
    do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 0);
    tf.x[8] = SYS_FCHMODAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/nope"; tf.x[2] = 0644; tf.x[3] = 0;
    do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == -ENOENT);
    fchmodat_ok = ok ? 1 : -1;
}
static void test_syscall_fchmodat(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    fchmodat_ok = 0;
    sched_init();
    thread_create(fchmodat_worker, 0, 1);
    while (fchmodat_ok == 0) { yield(); }
    KASSERT(fchmodat_ok == 1);
}

// utimensat + faccessat share fchmodat's existence-checked contract: 0 on a real
// path, -ENOENT on a missing one. (touch's timestamp update / access's R_W_X
// query are no-ops on a single-user system.)
static int atexist_ok;
static void atexist_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    int ok = 1;
    tf.x[8] = SYS_UTIMENSAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/hello.txt"; tf.x[2] = 0; tf.x[3] = 0;
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == 0);
    tf.x[8] = SYS_UTIMENSAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/nope"; tf.x[2] = 0; tf.x[3] = 0;
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == -ENOENT);
    tf.x[8] = SYS_FACCESSAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/hello.txt"; tf.x[2] = 0; tf.x[3] = 0;
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == 0);
    tf.x[8] = SYS_FACCESSAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/nope"; tf.x[2] = 0; tf.x[3] = 0;
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == -ENOENT);
    atexist_ok = ok ? 1 : -1;
}
static void test_syscall_utimensat_faccessat(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    atexist_ok = 0;
    sched_init();
    thread_create(atexist_worker, 0, 1);
    while (atexist_ok == 0) { yield(); }
    KASSERT(atexist_ok == 1);
}

// ftruncate(fd, length): we support truncation to ZERO only (our ext2/ramfs
// truncate frees all blocks -- it cannot extend), which is the common case
// (`: > file`). length 0 resets the file to empty and returns 0; any nonzero
// length is rejected with EINVAL rather than silently lying. /hello.txt starts
// at 15 bytes; after ftruncate(fd, 0) fstat must report size 0.
static int ftrunc_ok;
static void ftrunc_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    int ok = 1;
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/hello.txt"; tf.x[2] = O_RDWR; tf.x[3] = 0;
    do_syscall(&tf);
    long fd = (long)tf.x[0];
    ok = ok && (fd >= 3);
    // nonzero length is unsupported -> EINVAL, and must NOT alter the file.
    tf.x[8] = SYS_FTRUNCATE; tf.x[0] = (uint64_t)fd; tf.x[1] = 5;
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == -EINVAL);
    // truncate to empty.
    tf.x[8] = SYS_FTRUNCATE; tf.x[0] = (uint64_t)fd; tf.x[1] = 0;
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == 0);
    char sb[128];
    tf.x[8] = SYS_FSTAT; tf.x[0] = (uint64_t)fd; tf.x[1] = (uint64_t)(uintptr_t)sb;
    do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 0) && (*(int64_t *)(sb + 48) == 0);   // st_size == 0
    ftrunc_ok = ok ? 1 : -1;
}
static void test_syscall_ftruncate(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    ftrunc_ok = 0;
    sched_init();
    thread_create(ftrunc_worker, 0, 1);
    while (ftrunc_ok == 0) { yield(); }
    KASSERT(ftrunc_ok == 1);
}

// readlinkat(dirfd, path, buf, bufsiz): reads a symlink's target. Here we test
// the ERROR contract -- EINVAL for a non-symlink, ENOENT for a missing path --
// because the test FS (ramfs) has no symlinks. The success path (returning the
// target bytes) is proven end-to-end on ext2 by the integration check
// (`readlink /bin/ls` -> `busybox`, since the applets are real ext2 symlinks).
static int readlinkat_ok;
static void readlinkat_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    char buf[64];
    int ok = 1;
    tf.x[8] = SYS_READLINKAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/hello.txt";       // a regular file, not a link
    tf.x[2] = (uint64_t)(uintptr_t)buf; tf.x[3] = sizeof(buf);
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == -EINVAL);
    tf.x[8] = SYS_READLINKAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/nope";
    tf.x[2] = (uint64_t)(uintptr_t)buf; tf.x[3] = sizeof(buf);
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == -ENOENT);
    readlinkat_ok = ok ? 1 : -1;
}
static void test_syscall_readlinkat_errors(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    readlinkat_ok = 0;
    sched_init();
    thread_create(readlinkat_worker, 0, 1);
    while (readlinkat_ok == 0) { yield(); }
    KASSERT(readlinkat_ok == 1);
}

// sendfile(out_fd, in_fd, off, count): copy bytes directly between two
// descriptors in the kernel. musl's `cat` uses it (falling back to read/write
// only on ENOSYS). With off=NULL it copies from in_fd's current position and
// advances it. We copy /hello.txt ("Hello, MyOSv2!\n", 15 bytes) into a fresh
// file and verify both the returned count and the destination contents.
static int sendfile_ok;
static void sendfile_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    int ok = 1;
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/hello.txt"; tf.x[2] = 0; tf.x[3] = 0;
    do_syscall(&tf); long in = (long)tf.x[0];
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/out"; tf.x[2] = O_CREAT | O_RDWR; tf.x[3] = 0;
    do_syscall(&tf); long out = (long)tf.x[0];
    ok = ok && (in >= 3) && (out >= 3);
    tf.x[8] = SYS_SENDFILE; tf.x[0] = (uint64_t)out; tf.x[1] = (uint64_t)in;
    tf.x[2] = 0; tf.x[3] = 100;                 // off=NULL, count >= file size
    do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 15);           // whole file copied
    // Read /out back and compare.
    struct file *f = vfs_open("/out");
    char buf[16] = {0};
    ok = ok && f && (vfs_read(f, buf, 15) == 15) && bytes_eq(buf, "Hello, MyOSv2!\n", 15);
    if (f) { vfs_close(f); }
    sendfile_ok = ok ? 1 : -1;
}
static void test_syscall_sendfile(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    sendfile_ok = 0;
    sched_init();
    thread_create(sendfile_worker, 0, 1);
    while (sendfile_ok == 0) { yield(); }
    KASSERT(sendfile_ok == 1);
}

// The console (stdin/stdout/stderr) lives in NULL fd-table slots ("NULL means
// the UART"). A shell redirection SAVES the original stdio fd by duplicating it
// to a high slot, then restores it afterwards -- so a NULL console slot MUST be
// duplicatable into a real handle. This is the kernel-level linchpin of busybox
// redirection (proven end-to-end by the integration check). We verify both save
// paths ash uses: dup3 to an exact slot, and fcntl(F_DUPFD).
static int condup_ok;
static void condup_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    int ok = 1;
    struct file **fds = sched_current_fds();
    ok = ok && fds && !fds[1];                 // fd 1 starts NULL (console)
    // dup3(1 -> 10): duplicating the console must succeed and yield a real file.
    tf.x[8] = SYS_DUP3; tf.x[0] = 1; tf.x[1] = 10; tf.x[2] = 0;
    do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 10) && fds[10] != 0;
    // fcntl(0, F_DUPFD_CLOEXEC, 11): the save path ash ACTUALLY uses for
    // redirection. Must return a real new fd (a bogus 0 broke restore).
    tf.x[8] = SYS_FCNTL; tf.x[0] = 0; tf.x[1] = F_DUPFD_CLOEXEC; tf.x[2] = 11;
    do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 11) && fds[11] != 0;
    // The saved handle restores cleanly onto a console slot (dup3 back to 1).
    tf.x[8] = SYS_DUP3; tf.x[0] = 10; tf.x[1] = 1; tf.x[2] = 0;
    do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 1) && fds[1] != 0;
    condup_ok = ok ? 1 : -1;
}
static void test_syscall_console_fd_dupable(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    condup_ok = 0;
    sched_init();
    thread_create(condup_worker, 0, 1);
    while (condup_ok == 0) { yield(); }
    KASSERT(condup_ok == 1);
}

// openat O_APPEND (`>>`): the open must position writes at end-of-file, not 0,
// or appending silently OVERWRITES from the start. Create /ap.txt = "abc", open
// it O_APPEND, write "de" -- the file must become "abcde", not "dec".
static int append_ok;
static void append_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    int ok = 1;
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/ap.txt"; tf.x[2] = O_CREAT | O_RDWR; tf.x[3] = 0;
    do_syscall(&tf); long fd = (long)tf.x[0];
    tf.x[8] = SYS_WRITE; tf.x[0] = (uint64_t)fd;
    tf.x[1] = (uint64_t)(uintptr_t)"abc"; tf.x[2] = 3; do_syscall(&tf);
    tf.x[8] = SYS_CLOSE; tf.x[0] = (uint64_t)fd; do_syscall(&tf);
    // Reopen with O_APPEND and write -- must land at the end.
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/ap.txt"; tf.x[2] = O_APPEND | O_RDWR; tf.x[3] = 0;
    do_syscall(&tf); fd = (long)tf.x[0];
    ok = ok && (fd >= 3);
    tf.x[8] = SYS_WRITE; tf.x[0] = (uint64_t)fd;
    tf.x[1] = (uint64_t)(uintptr_t)"de"; tf.x[2] = 2; do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 2);
    struct file *f = vfs_open("/ap.txt");
    char buf[8] = {0};
    ok = ok && f && (vfs_read(f, buf, 5) == 5) && bytes_eq(buf, "abcde", 5);
    if (f) { vfs_close(f); }
    append_ok = ok ? 1 : -1;
}
static void test_syscall_open_append(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    append_ok = 0;
    sched_init();
    thread_create(append_worker, 0, 1);
    while (append_ok == 0) { yield(); }
    KASSERT(append_ok == 1);
}

// mkdirat(dirfd, path, mode): the filesystem `create` op already handles VN_DIR
// (lays down `.`/`..`, bumps link counts), so mkdirat just resolves the path and
// asks the VFS to create a directory. We verify the result is a real directory,
// a second mkdir of the same path is -EEXIST, and a relative path lands in cwd.
static int mkdir_ok;
static void mkdir_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    int ok = 1;
    tf.x[8] = SYS_MKDIRAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/d"; tf.x[2] = 0755;
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == 0);
    struct vnode *vn = vfs_lookup("/d");
    ok = ok && vn && (vn->type == VN_DIR);
    // Second create of the same name must fail with EEXIST, not duplicate it.
    tf.x[8] = SYS_MKDIRAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/d"; tf.x[2] = 0755;
    do_syscall(&tf); ok = ok && ((long)tf.x[0] == -EEXIST);
    // A file can then be created inside the new directory.
    tf.x[8] = SYS_OPENAT; tf.x[0] = (uint64_t)AT_FDCWD;
    tf.x[1] = (uint64_t)(uintptr_t)"/d/f"; tf.x[2] = O_CREAT | O_RDWR; tf.x[3] = 0;
    do_syscall(&tf); ok = ok && ((long)tf.x[0] >= 3);
    ok = ok && (vfs_lookup("/d/f") != 0);
    mkdir_ok = ok ? 1 : -1;
}
static void test_syscall_mkdirat(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    mkdir_ok = 0;
    sched_init();
    thread_create(mkdir_worker, 0, 1);
    while (mkdir_ok == 0) { yield(); }
    KASSERT(mkdir_ok == 1);
}

// --- PTY line discipline -------------------------------------------------
//
// A PTY is a tty with a REAL line discipline (unlike the UART console, which
// only fakes one). These pin the behaviours vi/less/the shell depend on. They
// pre-feed input and read with data already present, so the blocking read paths
// return immediately -- no threads needed.

// Compare the first n bytes of buf against a C string literal.
static int pty_buf_eq(const char *buf, int n, const char *want)
{
    int wl = 0; while (want[wl]) { wl++; }
    if (n != wl) { return 0; }
    for (int i = 0; i < n; i++) { if (buf[i] != want[i]) { return 0; } }
    return 1;
}

// Cooked mode: typed bytes are buffered and delivered as a whole line on RET.
static void test_pty_cooked_line(void)
{
    pmm_init(); kheap_init();
    struct pty *p = pty_alloc();
    pty_master_write(p, "hi\n", 3);          // user types "hi" + RET
    char b[16];
    int n = pty_slave_read(p, b, sizeof(b)); // program reads one line
    KASSERT(n == 3);
    KASSERT(pty_buf_eq(b, n, "hi\n"));
}

// Cooked mode: ERASE (DEL) rubs out the last pending char before commit.
static void test_pty_cooked_erase(void)
{
    pmm_init(); kheap_init();
    struct pty *p = pty_alloc();
    pty_master_write(p, "ab\x7f" "c\n", 5);  // a b <DEL> c RET -> "ac"
    char b[16];
    int n = pty_slave_read(p, b, sizeof(b));
    KASSERT(pty_buf_eq(b, n, "ac\n"));
}

// Cooked + ECHO: typed bytes bounce back to the screen (output ring), with NL
// expanded to CR-NL so the emulator returns the cursor to column 0.
static void test_pty_echo(void)
{
    pmm_init(); kheap_init();
    struct pty *p = pty_alloc();
    pty_master_write(p, "hi\n", 3);
    char b[16];
    int n = pty_master_read(p, b, sizeof(b)); // read the echo
    KASSERT(pty_buf_eq(b, n, "hi\r\n"));
}

// Raw mode (vi): ICANON/ECHO cleared -> every byte delivered immediately,
// unbuffered, and NOT echoed.
static void test_pty_raw_passthrough(void)
{
    pmm_init(); kheap_init();
    struct pty *p = pty_alloc();
    p->tio.c_lflag = 0;                       // raw: no ICANON, ECHO, ISIG
    pty_master_write(p, "hi", 2);             // no newline needed
    char b[16];
    int n = pty_slave_read(p, b, sizeof(b));
    KASSERT(n == 2);
    KASSERT(pty_buf_eq(b, n, "hi"));
    KASSERT(!pty_master_readable(p));         // nothing echoed
}

// Output OPOST|ONLCR: a cooked program's bare '\n' reaches the emulator as
// '\r\n'.
static void test_pty_output_onlcr(void)
{
    pmm_init(); kheap_init();
    struct pty *p = pty_alloc();
    pty_slave_write(p, "a\nb", 3);
    char b[16];
    int n = pty_master_read(p, b, sizeof(b));
    KASSERT(pty_buf_eq(b, n, "a\r\nb"));
}

// ISIG: the INTR char (^C) is consumed by the discipline (not delivered as data
// and not echoed). With no foreground pgrp set, no signal is sent.
static void test_pty_intr_consumed(void)
{
    pmm_init(); kheap_init();
    struct pty *p = pty_alloc();              // ISIG on by default, fg_pgrp 0
    pty_master_write(p, "\x03", 1);           // ^C
    KASSERT(!pty_slave_readable(p));          // not delivered to the program
    KASSERT(!pty_master_readable(p));         // not echoed to the screen
}

// openpty syscall + ioctl routing, end to end through do_syscall(): the slave
// answers TCGETS (so isatty() is true) and a TCSETS that clears ICANON REALLY
// switches the line discipline to raw -- the behaviour vi depends on.
static volatile int openpty_ok;
static void openpty_worker(void *a)
{
    (void)a;
    struct trapframe tf;
    int ok = 1;
    int ufd[2] = { -1, -1 };                 // {master, slave}
    tf.x[8] = SYS_OPENPT; tf.x[0] = (uint64_t)(uintptr_t)ufd; do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 0);
    ok = ok && (ufd[0] >= 3) && (ufd[1] >= 3) && (ufd[0] != ufd[1]);
    int master = ufd[0], slave = ufd[1];

    // TCGETS on the slave succeeds and reports the cooked default (ICANON set).
    struct pty_termios t;
    tf.x[8] = SYS_IOCTL; tf.x[0] = (uint64_t)slave; tf.x[1] = TCGETS;
    tf.x[2] = (uint64_t)(uintptr_t)&t; do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 0) && ((t.c_lflag & T_ICANON) != 0);

    // Switch the slave to raw by clearing the local flags; TCSETS must store it.
    t.c_lflag = 0;
    tf.x[8] = SYS_IOCTL; tf.x[0] = (uint64_t)slave; tf.x[1] = TCSETS;
    tf.x[2] = (uint64_t)(uintptr_t)&t; do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 0);

    // In raw mode one byte written to the master is immediately readable on the
    // slave -- no newline, no line buffering.
    char wb = 'R';
    tf.x[8] = SYS_WRITE; tf.x[0] = (uint64_t)master;
    tf.x[1] = (uint64_t)(uintptr_t)&wb; tf.x[2] = 1; do_syscall(&tf);
    char rb = 0;
    tf.x[8] = SYS_READ; tf.x[0] = (uint64_t)slave;
    tf.x[1] = (uint64_t)(uintptr_t)&rb; tf.x[2] = 1; do_syscall(&tf);
    ok = ok && ((long)tf.x[0] == 1) && (rb == 'R');

    openpty_ok = ok ? 1 : -1;
}
static void test_syscall_openpty(void)
{
    pmm_init(); kheap_init();
    fs_fresh();
    openpty_ok = 0;
    sched_init();
    thread_create(openpty_worker, 0, 1);
    while (openpty_ok == 0) { yield(); }
    KASSERT(openpty_ok == 1);
}

// The registry of all tests.
static const struct ktest tests[] = {
    { "lm: arithmetic",                  test_lm_arithmetic },
    { "lm: lists + cons/car/cdr",        test_lm_lists },
    { "lm: conditionals",                test_lm_conditionals },
    { "lm: let + setq",                  test_lm_let_and_setq },
    { "lm: defun + recursion",           test_lm_defun_and_recursion },
    { "lm: closures",                    test_lm_closures },
    { "lm: macros",                      test_lm_macros },
    { "lm: higher-order (mapcar/apply)", test_lm_higher_order },
    { "lm: strings + type-of",           test_lm_strings },
    { "lm: GC keeps roots, frees rest",  test_lm_gc_keeps_roots },
    { "lm: error recovery",              test_lm_error_recovery },
    { "lm: errors go to lm_cur_out",     test_lm_error_goes_to_cur_out },
    { "lm: rest params + | symbol",      test_lm_rest_params },
    { "lm: eval primitive",              test_lm_eval_primitive },
    { "lm: global bound to nil",         test_lm_global_nil_binding },
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
    { "console: Ctrl-C signals fg pgrp",  test_tty_intr_signals_fg_pgrp },
    { "syscall: write returns len",       test_syscall_write_returns_len },
    { "syscall: unknown returns -1",      test_syscall_unknown },
    { "syscall: yield returns 0",         test_syscall_yield },
    { "syscall: result written to x0",    test_syscall_return_in_x0 },
    { "syscall: getpid returns id",       test_syscall_getpid },
    { "syscall: sleep blocks N ticks",    test_syscall_sleep_blocks },
    { "syscall: exit ends thread",        test_syscall_exit_ends_thread },
    { "ioctl: TCGETS is a tty",           test_ioctl_tcgets_is_tty },
    { "syscall: getppid",                 test_getppid_returns_parent },
    { "syscall: fcntl F_GETFL",           test_fcntl_getfl_rdwr },
    { "syscall: clock_gettime",           test_clock_gettime_ok },
    { "syscall: dup3 dups onto exact fd", test_syscall_dup3 },
    { "syscall: pipe2 round-trips",       test_syscall_pipe2 },
    { "syscall: nanosleep blocks",        test_syscall_nanosleep_blocks },
    { "syscall: fchmodat existence",      test_syscall_fchmodat },
    { "syscall: utimensat + faccessat",   test_syscall_utimensat_faccessat },
    { "syscall: ftruncate to zero",       test_syscall_ftruncate },
    { "syscall: readlinkat errors",       test_syscall_readlinkat_errors },
    { "syscall: sendfile copies bytes",   test_syscall_sendfile },
    { "syscall: console fd is dup-able",  test_syscall_console_fd_dupable },
    { "syscall: openat O_APPEND",         test_syscall_open_append },
    { "syscall: mkdirat makes a dir",     test_syscall_mkdirat },
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
    { "vfs: truncate resets file",        test_vfs_truncate },
    { "vfs: readdir lists entries",       test_vfs_readdir_lists },
    { "vfs: lookup missing -> null",      test_vfs_lookup_missing },
    { "vfs: nested directory",            test_vfs_nested_dir },
    { "vfs: relative path from root",     test_vfs_lookup_relative },
    { "fs: seed provides /hello.txt",     test_seed_fs_provides_hello },
    { "fd: open returns fd",              test_fd_open_returns_fd },
    { "fd: read syscall",                 test_fd_read_syscall },
    { "fd: open missing -> -1",           test_fd_open_missing },
    { "fd: close then reuse",             test_fd_close_reuse },
    { "cwd: chdir + getcwd + relative",   test_cwd_chdir_getcwd },
    { "getdents64: lists directory",      test_getdents64_lists },
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
    { "pipe: fds get a clean ->sock",     test_pipe_file_sock_cleared },
    { "pipe: read EOF when no writers",   test_pipe_eof },
    { "pipe: write -1 when no readers",   test_pipe_broken },
    { "pipe: ring buffer wraps",          test_pipe_wraps },
    { "pipe: reader blocks, not spins",   test_pipe_read_blocks_not_spins },
    { "pty: cooked line on RET",          test_pty_cooked_line },
    { "pty: cooked ERASE edits line",     test_pty_cooked_erase },
    { "pty: ECHO bounces to screen",      test_pty_echo },
    { "pty: raw mode passthrough",        test_pty_raw_passthrough },
    { "pty: output ONLCR maps NL",        test_pty_output_onlcr },
    { "pty: ISIG consumes ^C",            test_pty_intr_consumed },
    { "pty: openpty + raw via ioctl",     test_syscall_openpty },
    { "poll: pipe readiness scan",        test_poll_pipe_readiness },
    { "tcp: cc slow start",               test_tcp_cc_slow_start },
    { "tcp: cc avoidance + loss",         test_tcp_cc_avoidance_and_loss },
    { "tcp: RST reply fields",            test_tcp_rst_fields },
    { "tcp: next segment (Nagle/window)", test_tcp_next_seg },
    { "udp: transmit checksum",           test_udp_checksum },
    { "dhcp: parse OFFER/ACK",            test_dhcp_parse },
    { "dhcp: lease renewal action",       test_dhcp_lease_action },
    { "file: refcount dup/close",         test_file_refcount },
    { "sig: kill sets pending",           test_kill_sets_pending },
    { "sig: kill by pid",                 test_kill_by_pid },
    { "sig: default vs handler action",   test_sig_default_vs_handler },
    { "sig: rt_sigaction installs handler", test_rt_sigaction_installs },
    { "sig: process groups (kill -pgid)", test_process_groups },
    { "sig: kill/setpgid linux numbers",  test_kill_setpgid_linux_numbers },
    { "block: disk present",              test_block_present },
    { "block: write then read sector",    test_block_write_read },
    { "block: two sectors independent",   test_block_two_sectors },
    { "ext2: mount root is dir",          test_ext2_mount_root_is_dir },
    { "ext2: read small file",            test_ext2_read_small_file },
    { "ext2: read large file via indirect", test_ext2_read_indirect },
    { "ext2: readdir lists /test",        test_ext2_readdir_test_dir },
    { "ext2: create + write + read",      test_ext2_create_write_read },
    { "ext2: write grows past direct (indirect alloc)", test_ext2_write_grows_indirect },
    { "ext2: truncate resets",            test_ext2_truncate_resets },
    { "ext2: unlink",                     test_ext2_unlink },
    { "ext2: persists across remount",    test_ext2_persists_remount },
    { "ext2: symlink follows to target",  test_ext2_symlink_follows },
    { "ext2: symlink create",             test_ext2_symlink_create },
    { "ext2: rename moves a file",        test_ext2_rename_moves },
    { "ext2: hard link shares inode",     test_ext2_hardlink },
    { "vfs: mount at /disk",              test_vfs_mount_at },
    { "input: two devices present",       test_input_devices_present },
    { "input: driver present",            test_input_present },
    { "input: poll drains injected event", test_input_poll_drain },
    { "syscall: input_read drains event", test_syscall_input_read },
    { "rd: gap buffer insert/delete/read", test_rd_gap_buffer },
    { "rd: single window layout",         test_rd_single_window_layout },
    { "rd: line wrap",                    test_rd_line_wrap },
    { "rd: modeline shows mode name",     test_rd_modeline_mode_name },
    { "rd: scroll follows point",         test_rd_scroll_follows_point },
    { "rd: delete other windows",         test_rd_delete_other },
    { "rd: split below + other window",   test_rd_split_below },
    { "rd: split right",                  test_rd_split_right },
    { "rd: damage confined to edit",      test_rd_damage_minimal },
    { "rd: glyphs hit the framebuffer",   test_rd_glyphs_hit_framebuffer },
    { "vm: shared mapping survives fork", test_shared_mapping_survives_fork },
    { "rd: minibuffer echo + selection", test_rd_minibuffer_echo },
    { "rd: surface buffer blit + damage", test_rd_surface_blit },
    { "gpu: device present",              test_gpu_present },
    { "gpu: scanout configured",          test_gpu_scanout },
    { "gpu: two seats, two resources",    test_gpu_two_seats },
    { "gpu: hardware cursor plane",       test_gpu_cursor_plane },
    { "seat: register/switch/release",    test_seat_logic },
    { "net: present + MAC",               test_net_present },
    { "net: ARP round-trip",              test_net_arp_roundtrip },
    { "net: internet checksum",           test_inet_checksum },
    { "net: ARP resolve gateway",         test_arp_resolve },
    { "net: ICMP ping gateway",           test_icmp_ping },
    { "net: resolve dotted-quad literal", test_resolve_dotted_quad },
    { "dns: build A query",               test_dns_build_query },
    { "dns: parse A record",              test_dns_parse_a_record },
    { "dns: skip CNAME to A",             test_dns_parse_skips_cname },
    { "dns: RCODE error -> fail",         test_dns_parse_rcode_error },
    { "dns: resolve localhost (live)",    test_dns_resolve_live },
    { "socket: udp queue + recvfrom",     test_socket_udp_queue },
    { "tcp: checksum verifies to 0",      test_tcp_checksum },
    { "tcp: reasm in-order run",          test_tcp_reasm_in_order },
    { "tcp: reasm out-of-order fill",     test_tcp_reasm_out_of_order },
    { "tcp: reasm wraps seq space",       test_tcp_reasm_wraps },
    { "tcp: reasm dup + out-of-window",   test_tcp_reasm_dup_and_out_of_window },
    { "tcp: rto first sample (RFC6298)",  test_tcp_rto_first_sample },
    { "tcp: rto backoff + clamp",         test_tcp_rto_backoff_and_clamp },
    { "tcp: flow-control windows",        test_tcp_flow_windows },
};

int run_self_tests(void)
{
    return ktest_run(tests, sizeof(tests) / sizeof(tests[0]));
}
