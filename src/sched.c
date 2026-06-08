// sched.c -- kernel threads + round-robin scheduler.
#include <stdint.h>
#include "sched.h"
#include "kheap.h"

#define STACK_SIZE (16 * 1024)   // 16 KiB per-thread stack

static struct thread *current;   // the running thread (NULL until sched_init)
static int next_id;              // monotonic thread id

struct thread *thread_create(void (*fn)(void *), void *arg, int priority)
{
    struct thread *t = kmalloc(sizeof(struct thread));
    uint8_t *stack   = kmalloc(STACK_SIZE);

    // Zero the saved context so unused callee-saved slots start clean.
    uint8_t *c = (uint8_t *)&t->ctx;
    for (unsigned i = 0; i < sizeof(t->ctx); i++) {
        c[i] = 0;
    }

    t->stack     = stack;
    t->state     = THREAD_RUNNABLE;
    t->id        = next_id++;
    t->priority  = priority;
    t->wake_tick = 0;
    t->next      = 0;

    // Craft the initial context. On the first cpu_switch into this thread,
    // cpu_switch restores x19/x20 then `ret`s into lr -> thread_trampoline,
    // which calls fn(arg) on this stack. The stack grows down, so sp starts at
    // the TOP of the allocation (16-byte aligned by construction: 16 KiB block).
    t->ctx.sp  = (uint64_t)(uintptr_t)(stack + STACK_SIZE);
    t->ctx.lr  = (uint64_t)(uintptr_t)thread_trampoline;
    t->ctx.x19 = (uint64_t)(uintptr_t)fn;
    t->ctx.x20 = (uint64_t)(uintptr_t)arg;

    // Link at the ring tail (just before `current`), so threads run in creation
    // order within a priority. IRQ-masked: the timer must not walk a
    // half-updated ring.
    uint64_t flags = irq_save();
    if (current) {
        struct thread *tail = current;
        while (tail->next != current) {
            tail = tail->next;
        }
        tail->next = t;
        t->next = current;
    } else {
        t->next = t;   // no scheduler yet (only the create-only test hits this)
    }
    irq_restore(flags);
    return t;
}

// The boot/idle thread: represents whatever was running when sched_init() ran
// (kmain, or the test harness). Its context is filled in on the first switch.
static struct thread boot_thread;
static int started;
static int slice_left = SCHED_TIME_SLICE;   // ticks remaining for `current`

void sched_init(void)
{
    boot_thread.stack = 0;            // it already has the kernel boot stack
    boot_thread.state = THREAD_RUNNING;
    boot_thread.id    = 0;
    boot_thread.priority = 0;         // (becomes idle priority -1 in the next task)
    boot_thread.next  = &boot_thread; // a ring of one
    current  = &boot_thread;
    next_id  = 1;
    started  = 1;
    slice_left = SCHED_TIME_SLICE;   // boot thread starts with a full slice
}

int sched_started(void)
{
    return started;
}

// Advance the current thread's time slice by one tick. When it runs out, reset
// it and tell the caller (the timer IRQ) to reschedule. Decoupling this from the
// timer tick is the Linux model: a fast tick, a slower scheduling quantum.
int sched_tick(void)
{
    if (!started) {
        return 0;
    }
    if (--slice_left <= 0) {
        slice_left = SCHED_TIME_SLICE;
        return 1;       // slice used up -> preempt
    }
    return 0;
}

// Round-robin: switch to the next non-exited thread after `current`.
void schedule(void)
{
    struct thread *prev = current;
    struct thread *next = prev->next;
    while (next->state == THREAD_EXITED && next != prev) {
        next = next->next;
    }
    if (next == prev) {
        return;                       // nobody else runnable -- keep going
    }
    slice_left = SCHED_TIME_SLICE;    // the newly-running thread gets a full slice
    current = next;
    cpu_switch(&prev->ctx, &next->ctx);
}

void yield(void)
{
    uint64_t flags = irq_save();
    schedule();                       // cooperative: voluntarily give up the CPU
    irq_restore(flags);
}

void thread_exit(void)
{
    uint64_t flags = irq_save();
    current->state = THREAD_EXITED;   // tombstone; schedule() skips it
    schedule();                       // switch away; never returns here
    irq_restore(flags);               // unreachable
    for (;;) { }
}
