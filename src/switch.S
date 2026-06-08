// switch.S -- cooperative context switch + new-thread entry trampoline.
.section ".text"

// void cpu_switch(struct context *old /*x0*/, struct context *new /*x1*/);
// Save the current thread's callee-saved regs into *old, load *new, then `ret`
// -- which jumps to new->lr, i.e. resumes the other thread. Offsets match the
// struct context layout in sched.h.
.global cpu_switch
cpu_switch:
    mov     x2, sp
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]     // fp (x29), lr (x30)
    str     x2,       [x0, #96]     // sp

    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]
    ldr     x2,       [x1, #96]
    mov     sp, x2
    ret                             // -> new->lr

// Where a brand-new thread begins. thread_create stashed fn in x19 and arg in
// x20; cpu_switch restored them just before `ret`-ing here. We enable IRQs (an
// exception entry had masked them if we were preempted) so this thread can also
// be preempted, then call fn(arg). If fn returns, end the thread.
.global thread_trampoline
thread_trampoline:
    msr     daifclr, #2             // unmask IRQs for this thread
    mov     x0, x20                 // arg
    blr     x19                     // fn(arg)
    bl      thread_exit             // fn returned -> terminate
