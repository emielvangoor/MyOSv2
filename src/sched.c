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

// Temporary stub so switch.S's `bl thread_exit` links. The real implementation
// (which unlinks the thread and switches away) arrives with the scheduler.
void thread_exit(void)
{
    for (;;) {
        __asm__ volatile("wfi");
    }
}
