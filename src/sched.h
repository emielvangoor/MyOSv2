// sched.h -- kernel threads and the priority round-robin scheduler.
// =================================================================
//
// A thread is an independent flow of execution with its own stack and saved
// register state. The scheduler runs the highest-priority RUNNABLE thread,
// round-robin among equal priorities, with a 1 ms tick and a 10 ms time slice.
#pragma once
#include <stdint.h>

// Callee-saved CPU state for a cooperative switch (offsets MUST match switch.S).
struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28; // @0 .. @72
    uint64_t fp;   // x29   @80
    uint64_t lr;   // x30   @88
    uint64_t sp;   //       @96
};

enum thread_state {
    THREAD_RUNNABLE,   // ready to run
    THREAD_RUNNING,    // currently on the CPU
    THREAD_SLEEPING,   // blocked until wake_tick
    THREAD_EXITED      // finished (a tombstone the scheduler skips)
};

struct thread {
    struct context ctx;    // MUST be first: switch.S uses &thread (== &thread->ctx)
    uint8_t *stack;        // kmalloc'd stack base (NULL for the idle thread)
    enum thread_state state;
    int id;
    int priority;          // higher number = more important
    uint64_t wake_tick;    // jiffy to wake at (when SLEEPING)
    struct thread *next;   // circular run-queue link
};

// Assembly (switch.S):
void cpu_switch(struct context *old, struct context *newc);
void thread_trampoline(void);
void user_entry_trampoline(void);   // assembly (usermode.S): drops a thread to EL0

// Scheduler:
void sched_init(void);                                            // register idle thread
struct thread *thread_create(void (*fn)(void *), void *arg, int priority);
struct thread *thread_create_user(void (*user_fn)(void), int priority); // EL0 thread
void yield(void);                                                 // cooperative switch
void schedule(void);                                             // pick highest-prio + switch
void thread_exit(void);                                          // end current thread
int  sched_started(void);
int  sched_current_id(void);                                     // id of running thread (-1 if none)
int  sched_tick(void);                                           // per-tick: wake sleepers + slice
void sleep_ticks(uint64_t ticks);                                // block for N ticks
void sleep_ms(uint64_t ms);                                      // block for N ms (1 ms == 1 tick)

// Length of a thread's time slice, in timer ticks (Linux-style quantum).
#define SCHED_TIME_SLICE 10

// Critical-section helpers: mask IRQs so the timer can't corrupt the run-queue
// while a thread modifies it. irq_save() returns the old DAIF for irq_restore().
static inline uint64_t irq_save(void)
{
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");  // set the I (IRQ) mask bit
    return daif;
}

static inline void irq_restore(uint64_t daif)
{
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}
