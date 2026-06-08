// usermode.S -- drop a thread to EL0 (user mode).
.section ".text"

// void enter_user(uint64_t entry /*x0*/, uint64_t user_sp /*x1*/);
// Set the user stack, the return address, and a processor state of "EL0 with
// IRQs enabled", then `eret` -- which lands at `entry` running unprivileged.
.global enter_user
enter_user:
    msr     sp_el0, x1           // user stack pointer (SP_EL0)
    msr     elr_el1, x0          // where eret resumes
    mov     x2, #0               // SPSR: M=EL0t, DAIF=0 (IRQs enabled)
    msr     spsr_el1, x2
    eret                         // -> EL0

// The initial lr of a user thread. cpu_switch restored the user entry into x19
// and the user stack into x20; hand them to enter_user (which never returns).
.global user_entry_trampoline
user_entry_trampoline:
    mov     x0, x19
    mov     x1, x20
    bl      enter_user
    // not reached
