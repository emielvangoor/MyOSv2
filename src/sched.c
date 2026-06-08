// sched.c -- kernel threads + round-robin scheduler.
#include <stdint.h>
#include "sched.h"
#include "kheap.h"

#define STACK_SIZE (16 * 1024)   // 16 KiB per-thread stack

static struct thread *current;   // the running thread (NULL until sched_init)
static int next_id;              // monotonic thread id

struct thread *thread_create(void (*fn)(void *), void *arg)
{
    struct thread *t = kmalloc(sizeof(struct thread));
    uint8_t *stack   = kmalloc(STACK_SIZE);

    // Zero the saved context so unused callee-saved slots start clean.
    uint8_t *c = (uint8_t *)&t->ctx;
    for (unsigned i = 0; i < sizeof(t->ctx); i++) {
        c[i] = 0;
    }

    t->stack = stack;
    t->state = THREAD_RUNNABLE;
    t->id    = next_id++;
    t->next  = 0;

    // Craft the initial context. On the first cpu_switch into this thread,
    // cpu_switch restores x19/x20 then `ret`s into lr -> thread_trampoline,
    // which calls fn(arg) on this stack. The stack grows down, so sp starts at
    // the TOP of the allocation (16-byte aligned by construction: 16 KiB block).
    t->ctx.sp  = (uint64_t)(uintptr_t)(stack + STACK_SIZE);
    t->ctx.lr  = (uint64_t)(uintptr_t)thread_trampoline;
    t->ctx.x19 = (uint64_t)(uintptr_t)fn;
    t->ctx.x20 = (uint64_t)(uintptr_t)arg;

    // If the scheduler is running, link this thread at the TAIL of the ring
    // (just before `current`), so threads run in creation order.
    if (current) {
        struct thread *tail = current;
        while (tail->next != current) {
            tail = tail->next;
        }
        tail->next = t;
        t->next = current;
    }
    return t;
}

// The boot/idle thread: represents whatever was running when sched_init() ran
// (kmain, or the test harness). Its context is filled in on the first switch.
static struct thread boot_thread;
static int started;

void sched_init(void)
{
    boot_thread.stack = 0;            // it already has the kernel boot stack
    boot_thread.state = THREAD_RUNNING;
    boot_thread.id    = 0;
    boot_thread.next  = &boot_thread; // a ring of one
    current  = &boot_thread;
    next_id  = 1;
    started  = 1;
}

int sched_started(void)
{
    return started;
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
    current = next;
    cpu_switch(&prev->ctx, &next->ctx);
}

void yield(void)
{
    schedule();                       // cooperative: voluntarily give up the CPU
}

void thread_exit(void)
{
    current->state = THREAD_EXITED;

    // Unlink `current` from the ring (find its predecessor first).
    struct thread *p = current;
    while (p->next != current) {
        p = p->next;
    }
    p->next = current->next;

    // Switch away forever. `next` is a surviving thread; the exited thread's
    // saved context is never used again (its stack leaks -- see notes).
    struct thread *prev = current;
    struct thread *next = current->next;
    current = next;
    cpu_switch(&prev->ctx, &next->ctx);
    // not reached
}
