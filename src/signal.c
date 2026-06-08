// signal.c -- posting, default actions, and delivery of signals.
// ==============================================================

#include <stdint.h>
#include "signal.h"
#include "sched.h"
#include "exceptions.h"

// Post a signal: set the pending bit, and wake the target if it's blocked.
void signal_send(struct thread *t, int sig)
{
    if (!t || sig <= 0 || sig >= NSIG) { return; }
    uint64_t f = irq_save();
    t->sig_pending |= (1ull << sig);
    // Unblock a sleeping/blocked thread so the signal can be delivered (EINTR):
    // a timed sleep, or a thread parked on a wait-channel (sched_block).
    if (t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED) {
        t->state = THREAD_RUNNABLE;
        t->wait_chan = 0;
    }
    irq_restore(f);
}

// The handler address for `sig`, or 0 for the default action (terminate).
// SIGKILL can never be caught.
uint64_t signal_action(struct thread *t, int sig)
{
    if (sig == SIGKILL || sig <= 0 || sig >= NSIG) { return 0; }
    return (uint64_t)(uintptr_t)t->sig_handler[sig];
}

// Deliver one pending signal as we return to EL0. With no handler we terminate
// the process; with a handler we redirect the trap frame to run it in user mode,
// having pushed the interrupted context onto the user stack for SYS_SIGRETURN.
void signals_deliver(struct trapframe *tf)
{
    struct thread *t = sched_current();
    if (!t || !t->sig_pending) { return; }

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(t->sig_pending & (1ull << sig))) { continue; }
        t->sig_pending &= ~(1ull << sig);

        uint64_t h = signal_action(t, sig);
        if (!h) {
            thread_exit(128 + sig);          // default action: terminate (no return)
        }

        // Save the interrupted frame on the user stack (16-aligned), then point
        // the trap frame at the handler: x0=sig, lr=trampoline (-> SYS_SIGRETURN).
        uint64_t sp = (tf->sp_el0 - sizeof(struct trapframe)) & ~15ull;
        const uint64_t *src = (const uint64_t *)tf;
        uint64_t *dst = (uint64_t *)(uintptr_t)sp;
        for (unsigned i = 0; i < sizeof(struct trapframe) / 8; i++) { dst[i] = src[i]; }

        tf->sp_el0 = sp;
        tf->x[0]   = (uint64_t)sig;
        tf->elr    = h;
        tf->x[30]  = t->sig_tramp;            // handler `ret`s into __sigreturn
        return;                                // one signal per return to EL0
    }
}
