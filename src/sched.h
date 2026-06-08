// sched.h -- kernel threads and the round-robin scheduler.
// =======================================================
//
// A thread is an independent flow of execution with its own stack and saved
// register state. Switching threads = save the current registers, load another
// thread's, and resume. Do it on the timer and it looks like things run at once.
#pragma once
#include <stdint.h>

// Callee-saved CPU state for a cooperative switch. A normal function call may
// clobber x0-x18, so we only need to preserve x19-x30 + sp. The byte offsets
// here MUST match switch.S (cpu_switch stores/loads at fixed offsets).
struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28; // @0 .. @72
    uint64_t fp;   // x29   @80
    uint64_t lr;   // x30   @88  (cpu_switch's `ret` resumes here)
    uint64_t sp;   //       @96
};

enum thread_state { THREAD_RUNNABLE, THREAD_RUNNING, THREAD_EXITED };

struct thread {
    struct context ctx;    // MUST be first: switch.S uses &thread (== &thread->ctx)
    uint8_t *stack;        // kmalloc'd stack base (NULL for the boot thread)
    enum thread_state state;
    int id;
    struct thread *next;   // circular run-queue link
};

// Assembly (switch.S):
void cpu_switch(struct context *old, struct context *newc);
void thread_trampoline(void);   // the initial `lr` of every new thread

// Scheduler:
void sched_init(void);                                   // register the boot thread
struct thread *thread_create(void (*fn)(void *), void *arg);
void yield(void);                                        // cooperative switch
void schedule(void);                                     // pick next + switch
void thread_exit(void);                                  // end current thread
int  sched_started(void);                                // has sched_init run?

// Length of a thread's time slice, in timer ticks (Linux-style quantum).
#define SCHED_TIME_SLICE 10

// Called once per timer tick. Returns 1 when the current thread's time slice has
// expired (a reschedule is due) and resets the slice; returns 0 otherwise (or if
// the scheduler hasn't started).
int sched_tick(void);
