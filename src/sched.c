// sched.c -- kernel threads + round-robin scheduler.
#include <stdint.h>
#include "sched.h"
#include "kheap.h"
#include "exceptions.h"   // struct trapframe (for fork)
#include "vfs.h"          // file_dup (share fds across fork)

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
    t->as        = 0;            // kernel thread: no user address space
    for (int i = 0; i < 16; i++) { t->fds[i] = 0; }
    t->parent      = current;    // the creating thread is our parent
    t->exit_status = 0;
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

struct file **sched_current_fds(void)
{
    return current ? current->fds : 0;
}

struct addrspace *sched_current_as(void)
{
    return current ? current->as : 0;
}

extern void fork_ret(void);   // vectors.S: a forked child's first entry

// fork: build a child thread that is a copy-on-write clone of the current one.
// It resumes at the parent's `svc` with x0 = 0 (via fork_ret + a copied trap
// frame). Returns the child's pid (to the parent).
int sched_fork(struct trapframe *parent_tf)
{
    struct thread *t = kmalloc(sizeof(struct thread));
    uint8_t *kstack  = kmalloc(STACK_SIZE);

    uint8_t *c = (uint8_t *)&t->ctx;
    for (unsigned i = 0; i < sizeof(t->ctx); i++) { c[i] = 0; }

    t->stack     = kstack;
    t->state     = THREAD_RUNNABLE;
    t->id        = next_id++;
    t->priority  = current->priority;
    t->wake_tick = 0;
    t->as        = as_clone(current->as);            // copy-on-write address space
    for (int i = 0; i < 16; i++) {
        t->fds[i] = current->fds[i];                 // share open files...
        if (t->fds[i]) { file_dup(t->fds[i]); }      // ...with a reference each
    }
    t->parent      = current;                        // the forking thread is our parent
    t->exit_status = 0;
    t->next      = 0;

    // Place a copy of the parent's trap frame at the top of the child's kernel
    // stack, with x0 = 0; fork_ret (= kernel_exit) will eret it to EL0.
    uint64_t top = ((uint64_t)(uintptr_t)(kstack + STACK_SIZE)) & ~15UL;
    uint64_t tf_addr = top - 272;                    // sizeof(struct trapframe), 16-aligned
    struct trapframe *ctf = (struct trapframe *)(uintptr_t)tf_addr;
    const uint64_t *pw = (const uint64_t *)parent_tf;   // copy word by word (no memcpy)
    uint64_t *cw = (uint64_t *)ctf;
    for (unsigned i = 0; i < sizeof(struct trapframe) / 8; i++) { cw[i] = pw[i]; }
    ctf->x[0] = 0;
    t->ctx.sp = tf_addr;
    t->ctx.lr = (uint64_t)(uintptr_t)fork_ret;

    uint64_t flags = irq_save();
    struct thread *tail = current;
    while (tail->next != current) { tail = tail->next; }
    tail->next = t;
    t->next = current;
    irq_restore(flags);
    return t->id;
}

// Create a thread that starts at EL0 in its OWN address space, running the
// program image `img` (len bytes). Its kmalloc stack is the kernel stack
// (SP_EL1, for traps); its user code/stack/data live in the private address
// space built by as_create_image(). The initial context lands in
// user_entry_trampoline, which drops to EL0 at the user entry VA.
struct thread *thread_create_image(const void *img, uint64_t len, int priority)
{
    struct thread *t = kmalloc(sizeof(struct thread));
    uint8_t *kstack  = kmalloc(STACK_SIZE);   // kernel stack (SP_EL1, for traps)

    uint8_t *c = (uint8_t *)&t->ctx;
    for (unsigned i = 0; i < sizeof(t->ctx); i++) {
        c[i] = 0;
    }

    uint64_t entry = 0;
    t->stack     = kstack;
    t->state     = THREAD_RUNNABLE;
    t->id        = next_id++;
    t->priority  = priority;
    t->wake_tick = 0;
    t->as        = as_create_elf(img, len, &entry); // ELF: its own private AS
    for (int i = 0; i < 16; i++) { t->fds[i] = 0; }
    t->parent      = current;                 // the spawning thread is our parent
    t->exit_status = 0;
    t->next      = 0;

    // The trampoline + kernel stack run at EL1; it then drops to EL0 at the ELF
    // entry point with the user stack -- both inside this process's address space.
    t->ctx.sp  = (uint64_t)(uintptr_t)(kstack + STACK_SIZE);  // kernel stack top (EL1)
    t->ctx.lr  = (uint64_t)(uintptr_t)user_entry_trampoline;  // runs at EL1
    t->ctx.x19 = entry;                                       // ELF entry (in its AS)
    t->ctx.x20 = USER_STACK_TOP;                              // user stack top (in its AS)

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
    boot_thread.as        = 0;            // the idle/kernel thread has no user AS
    for (int i = 0; i < 16; i++) { boot_thread.fds[i] = 0; }
    boot_thread.parent      = 0;          // the idle thread has no parent
    boot_thread.exit_status = 0;
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
    if (best->as) {
        as_switch(best->as);         // install the process's page tables (TTBR0)
    }
    cpu_switch(&prev->ctx, &best->ctx);
}

void yield(void)
{
    uint64_t flags = irq_save();
    schedule();                       // cooperative: voluntarily give up the CPU
    irq_restore(flags);
}

// End the current thread, recording an exit status. It becomes a ZOMBIE -- it
// stays in the run-ring (the scheduler skips it) until its parent reaps it via
// sched_wait(). If the parent is already blocked in wait(), wake it.
void thread_exit(int status)
{
    // Close the process's open files first, so e.g. a pipe's writer count drops
    // and a reader on the other end sees EOF. Refcounting keeps a file alive if a
    // forked relative still holds it.
    for (int i = 0; i < 16; i++) {
        if (current->fds[i]) { vfs_close(current->fds[i]); current->fds[i] = 0; }
    }

    uint64_t flags = irq_save();
    current->exit_status = status;
    current->state = THREAD_ZOMBIE;
    if (current->parent && current->parent->state == THREAD_SLEEPING) {
        current->parent->state = THREAD_RUNNABLE;   // a waiting parent can now reap us
    }
    schedule();                       // switch away; never returns here
    irq_restore(flags);               // unreachable
    for (;;) { }
}

// Rebind the running thread's address space (used by exec to install a new image).
void sched_set_current_as(struct addrspace *as)
{
    current->as = as;
}

// Wait for a child to exit, reap it, and return its pid (or -1 if the caller has
// no children). Reaping unlinks the zombie from the ring and frees its kernel
// stack, address space, and thread struct.
int sched_wait(int *status)
{
    for (;;) {
        uint64_t flags = irq_save();
        struct thread *prev = current;   // prev->next is the node under inspection
        struct thread *child = 0;        // a reapable zombie child
        struct thread *any = 0;          // any child at all (maybe still running)
        do {
            struct thread *n = prev->next;
            if (n->parent == current) {
                any = n;
                if (n->state == THREAD_ZOMBIE) { child = n; break; }
            }
            prev = n;
        } while (prev != current);

        if (child) {
            if (status) { *status = child->exit_status; }
            int pid = child->id;
            prev->next = child->next;             // unlink from the circular ring
            kfree(child->stack);
            if (child->as) { as_destroy(child->as); }
            kfree(child);
            irq_restore(flags);
            return pid;
        }
        if (!any) { irq_restore(flags); return -1; }   // no children to wait for

        current->state = THREAD_SLEEPING;   // block until a child exits (wakes us)
        schedule();
        irq_restore(flags);
    }
}
