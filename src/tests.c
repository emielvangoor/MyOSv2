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
    struct thread *t = thread_create(noop_thread, arg);

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
    thread_create(rr_worker, (void *)(uintptr_t)1);
    thread_create(rr_worker, (void *)(uintptr_t)2);

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
};

int run_self_tests(void)
{
    return ktest_run(tests, sizeof(tests) / sizeof(tests[0]));
}
