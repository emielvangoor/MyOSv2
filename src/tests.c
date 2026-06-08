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
#include "pmm.h"
#include "kheap.h"
#include "sched.h"
#include "syscall.h"
#include "vm.h"
#include "vfs.h"
#include "ramfs.h"

#define PAGE 0x1000UL

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

// --- System calls (do_syscall dispatch) ---

static void test_syscall_write_returns_len(void)
{
    struct trapframe tf;
    const char *s = "hello";
    tf.x[8] = SYS_WRITE;
    tf.x[0] = (uint64_t)(uintptr_t)s;
    tf.x[1] = 5;
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

static void test_as_code_is_shared(void)
{
    pmm_init(); kheap_init(); vm_init();
    struct addrspace *a = as_create();
    struct addrspace *b = as_create();
    KASSERT(as_translate(a, USER_CODE_VA) == as_translate(b, USER_CODE_VA));
    KASSERT(as_translate(a, USER_CODE_VA) != 0);
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
    { "syscall: write returns len",       test_syscall_write_returns_len },
    { "syscall: unknown returns -1",      test_syscall_unknown },
    { "syscall: yield returns 0",         test_syscall_yield },
    { "syscall: result written to x0",    test_syscall_return_in_x0 },
    { "syscall: getpid returns id",       test_syscall_getpid },
    { "syscall: sleep blocks N ticks",    test_syscall_sleep_blocks },
    { "syscall: exit ends thread",        test_syscall_exit_ends_thread },
    { "vm: user data is private",         test_as_data_is_private },
    { "vm: user code is shared",          test_as_code_is_shared },
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
};

int run_self_tests(void)
{
    return ktest_run(tests, sizeof(tests) / sizeof(tests[0]));
}
