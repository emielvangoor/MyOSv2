// sched.h -- kernel threads and the priority round-robin scheduler.
// =================================================================
//
// A thread is an independent flow of execution with its own stack and saved
// register state. The scheduler runs the highest-priority RUNNABLE thread,
// round-robin among equal priorities, with a 1 ms tick and a 10 ms time slice.
#pragma once
#include <stdint.h>
#include "vm.h"

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
    THREAD_SLEEPING,   // blocked until wake_tick (or until a child exits, for wait)
    THREAD_BLOCKED,    // blocked on a wait-channel until sched_wake() or a signal
    THREAD_EXITED,     // finished (a tombstone the scheduler skips)
    THREAD_ZOMBIE      // exited, awaiting reap by its parent (Phase 13)
};

struct thread {
    struct context ctx;    // MUST be first: switch.S uses &thread (== &thread->ctx)
    uint8_t *stack;        // kmalloc'd stack base (NULL for the idle thread)
    enum thread_state state;
    int id;
    int priority;          // higher number = more important
    uint64_t wake_tick;    // jiffy to wake at (when SLEEPING)
    void *wait_chan;       // wait-channel this thread is BLOCKED on (0 if none)
    struct addrspace *as;  // user address space (NULL for kernel threads)
    struct file *fds[16];  // open file table (a process). NULL = free.
    struct thread *parent; // who created us (NULL for the boot/idle thread)
    int exit_status;       // status passed to exit() (read by the parent's wait)
    uint64_t sig_pending;            // bitmask of posted-but-undelivered signals
    uint64_t (*sig_handler[32])(int);// per-signal user handler (NULL = default action)
    uint64_t sig_tramp;              // user trampoline a handler returns into
    struct thread *next;   // circular run-queue link
};

// Assembly (switch.S):
void cpu_switch(struct context *old, struct context *newc);
void thread_trampoline(void);
void user_entry_trampoline(void);   // assembly (usermode.S): drops a thread to EL0

// Scheduler:
void sched_init(void);                                            // register idle thread
struct file;   // from vfs.h (the fd table holds these)
struct thread *thread_create(void (*fn)(void *), void *arg, int priority);
struct thread *thread_create_image(const void *img, uint64_t len, int priority); // EL0 program
struct file  **sched_current_fds(void);   // the running thread's fd table
struct addrspace *sched_current_as(void); // the running thread's address space
struct trapframe;
int sched_fork(struct trapframe *parent_tf);   // fork: clone current; returns child pid
void yield(void);                                                 // cooperative switch
void schedule(void);                                             // pick highest-prio + switch
void thread_exit(int status);                                    // end current thread with a status
int  sched_wait(int *status);                                    // reap a zombie child; -1 if none
void sched_set_current_as(struct addrspace *as);                 // rebind current's address space (exec)
struct thread *sched_current(void);                              // the running thread
int  sched_kill(int pid, int sig);                               // post a signal to pid; -1 if no such pid
void sched_set_foreground(struct thread *t);                     // the thread Ctrl-C targets
struct thread *sched_foreground(void);
int  sched_started(void);
int  sched_current_id(void);                                     // id of running thread (-1 if none)
int  sched_tick(void);                                           // per-tick: wake sleepers + slice
void sleep_ticks(uint64_t ticks);                                // block for N ticks
void sleep_ms(uint64_t ms);                                      // block for N ms (1 ms == 1 tick)

// The V6 sleep/wakeup primitive. sched_block() puts the current thread to sleep
// on an arbitrary "wait-channel" (any address used as a key) until sched_wake()
// is called with the same channel -- or a signal is posted to it (EINTR). The
// caller MUST re-check its condition in a loop after sched_block() returns, since
// a wakeup is advisory (it may have been a signal, or another thread may have
// consumed the resource first).
void sched_block(void *chan);
void sched_wake(void *chan);

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
