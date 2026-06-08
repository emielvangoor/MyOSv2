// sched.c -- kernel threads + round-robin scheduler.
#include <stdint.h>
#include "sched.h"
#include "kheap.h"
#include "mmu.h"

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

// Create a thread that starts at EL0. It gets TWO stacks: its kmalloc stack is
// the kernel stack (SP_EL1, used when it traps), plus a separate user stack
// (SP_EL0). The initial context lands in user_entry_trampoline, which drops to
// EL0 at user_fn.
struct thread *thread_create_user(void (*user_fn)(void), int priority)
{
    struct thread *t = kmalloc(sizeof(struct thread));
    uint8_t *kstack  = kmalloc(STACK_SIZE);   // kernel stack (SP_EL1)
    uint8_t *ustack  = kmalloc(STACK_SIZE);   // user stack   (SP_EL0)

    uint8_t *c = (uint8_t *)&t->ctx;
    for (unsigned i = 0; i < sizeof(t->ctx); i++) {
        c[i] = 0;
    }

    t->stack     = kstack;
    t->state     = THREAD_RUNNABLE;
    t->id        = next_id++;
    t->priority  = priority;
    t->wake_tick = 0;
    t->next      = 0;

    // The trampoline + kernel stack run at EL1 (identity addresses). But the
    // user entry and user stack must be the EL0-accessible ALIAS addresses
    // (physical + USER_ALIAS_OFFSET), since EL0 can only touch the alias window.
    t->ctx.sp  = (uint64_t)(uintptr_t)(kstack + STACK_SIZE);  // kernel stack top (EL1)
    t->ctx.lr  = (uint64_t)(uintptr_t)user_entry_trampoline;  // runs at EL1
    t->ctx.x19 = (uint64_t)(uintptr_t)user_fn + USER_ALIAS_OFFSET;          // user entry (EL0 alias)
    t->ctx.x20 = (uint64_t)(uintptr_t)(ustack + STACK_SIZE) + USER_ALIAS_OFFSET; // user stack top (EL0 alias)

    uint64_t flags = irq_save();
    if (current) {
        struct thread *tail = current;
        while (tail->next != current) {
            tail = tail->next;
        }
        tail->next = t;
        t->next = current;
    } else {
        t->next = t;
    }
    irq_restore(flags);
    return t;
}

// The boot/idle thread: represents whatever was running when sched_init() ran
// (kmain, or the test harness). Its context is filled in on the first switch.
static struct thread boot_thread;
static int started;
static int slice_left = SCHED_TIME_SLICE;   // ticks remaining for `current`
static uint64_t jiffies;                    // ticks since sched_init (sleep clock)

void sched_init(void)
{
    // The boot context becomes the idle thread: always runnable, lowest
    // priority, so it only runs when every other thread is blocked.
    boot_thread.stack     = 0;            // it already has the kernel boot stack
    boot_thread.state     = THREAD_RUNNING;
    boot_thread.id        = 0;
    boot_thread.priority  = -1;           // below any created thread
    boot_thread.wake_tick = 0;
    boot_thread.next      = &boot_thread; // a ring of one
    current  = &boot_thread;
    next_id  = 1;
    started  = 1;
    slice_left = SCHED_TIME_SLICE;   // idle thread starts with a full slice
    jiffies  = 0;
}

int sched_started(void)
{
    return started;
}

int sched_current_id(void)
{
    return current ? current->id : -1;
}

// Advance the current thread's time slice by one tick. When it runs out, reset
// it and tell the caller (the timer IRQ) to reschedule. Decoupling this from the
// timer tick is the Linux model: a fast tick, a slower scheduling quantum.
int sched_tick(void)
{
    if (!started) {
        return 0;
    }
    jiffies++;

    // Wake any sleeper whose deadline has arrived. If a woken thread outranks
    // the current one, ask for an immediate reschedule so priority is prompt.
    int wake_preempt = 0;
    struct thread *t = current;
    do {
        if (t->state == THREAD_SLEEPING && t->wake_tick <= jiffies) {
            t->state = THREAD_RUNNABLE;
            if (t->priority > current->priority) {
                wake_preempt = 1;
            }
        }
        t = t->next;
    } while (t != current);

    if (wake_preempt) {
        slice_left = SCHED_TIME_SLICE;
        return 1;
    }
    if (--slice_left <= 0) {
        slice_left = SCHED_TIME_SLICE;
        return 1;       // slice used up -> preempt
    }
    return 0;
}

// Block the current thread for `ticks` timer ticks, then become runnable again.
void sleep_ticks(uint64_t ticks)
{
    uint64_t flags = irq_save();
    current->wake_tick = jiffies + ticks;
    current->state = THREAD_SLEEPING;   // schedule() will skip us
    schedule();                         // switch away; returns once we're woken
    irq_restore(flags);
}

void sleep_ms(uint64_t ms)
{
    sleep_ticks(ms);   // TIMER_HZ == 1000, so 1 ms == 1 tick
}

// Pick the highest-priority RUNNABLE thread, round-robin within a level, and
// switch to it. The idle thread is always RUNNABLE at the lowest priority, so a
// runnable thread always exists.
void schedule(void)
{
    // current yields the CPU unless the caller already parked it (SLEEPING/EXITED).
    if (current->state == THREAD_RUNNING) {
        current->state = THREAD_RUNNABLE;
    }

    // Scan the whole ring starting just after current. "Strictly greater" means
    // among equal top priorities we keep the FIRST one after current -> round
    // robin within the level.
    struct thread *best = 0;
    struct thread *t = current->next;
    do {
        if (t->state == THREAD_RUNNABLE) {
            if (!best || t->priority > best->priority) {
                best = t;
            }
        }
        t = t->next;
    } while (t != current->next);

    if (!best) {
        return;   // nothing runnable (shouldn't happen: idle is always runnable)
    }

    best->state = THREAD_RUNNING;
    if (best == current) {
        return;                      // current is still the best -- keep running
    }

    struct thread *prev = current;
    current = best;
    slice_left = SCHED_TIME_SLICE;   // fresh slice for the newly-running thread
    cpu_switch(&prev->ctx, &best->ctx);
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
