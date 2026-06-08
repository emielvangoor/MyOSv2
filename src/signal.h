// signal.h -- asynchronous signals (kill, handlers, default actions).
// ===================================================================
//
// A signal is a small integer posted to a process. The process either runs a
// handler it installed, or takes the default action (terminate). Signals are
// delivered when the process is about to return to user mode (EL0).
#pragma once
#include <stdint.h>

struct thread;
struct trapframe;

#define SIGINT   2     // interrupt (Ctrl-C)
#define SIGKILL  9     // forced termination (cannot be caught)
#define SIGTERM  15    // polite termination request
#define NSIG     32    // number of signal slots

// Post `sig` to thread `t` (set the pending bit; wake it if sleeping).
void     signal_send(struct thread *t, int sig);

// The action for `sig` on `t`: the handler's user address, or 0 for the default
// (terminate). SIGKILL always returns 0 (uncatchable).
uint64_t signal_action(struct thread *t, int sig);

// Deliver one pending signal (if any) as we return to EL0: terminate on the
// default action, or redirect the trap frame into the user handler.
void     signals_deliver(struct trapframe *tf);
