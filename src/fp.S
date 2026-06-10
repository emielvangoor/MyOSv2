// fp.S -- FPU enablement + per-thread FP state save/restore (the FPU phase).
// ==========================================================================
// The kernel itself is compiled -mgeneral-regs-only and NEVER touches the
// V registers -- which is exactly why context switch is the only place FP
// state needs saving: between switches, user FP state survives kernel
// excursions untouched. These are the only FP instructions on the kernel
// side, quarantined here with the arch extension enabled explicitly.
.arch armv8-a+fp+simd
.section ".text"

// void fpu_enable(void): stop trapping FP/SIMD at EL0 and EL1.
.global fpu_enable
fpu_enable:
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)      // FPEN = 0b11: no traps
    msr     cpacr_el1, x0
    isb
    ret

// void fp_save(void *area): FPCR+FPSR at offset 0, then 32 V regs --
// FPCR first because stp's signed immediate tops out at 504.
.global fp_save
fp_save:
    stp     q0,  q1,  [x0, #16]
    stp     q2,  q3,  [x0, #48]
    stp     q4,  q5,  [x0, #80]
    stp     q6,  q7,  [x0, #112]
    stp     q8,  q9,  [x0, #144]
    stp     q10, q11, [x0, #176]
    stp     q12, q13, [x0, #208]
    stp     q14, q15, [x0, #240]
    stp     q16, q17, [x0, #272]
    stp     q18, q19, [x0, #304]
    stp     q20, q21, [x0, #336]
    stp     q22, q23, [x0, #368]
    stp     q24, q25, [x0, #400]
    stp     q26, q27, [x0, #432]
    stp     q28, q29, [x0, #464]
    stp     q30, q31, [x0, #496]
    mrs     x1, fpcr
    mrs     x2, fpsr
    stp     x1, x2, [x0, #0]
    ret

// void fp_restore(const void *area)
.global fp_restore
fp_restore:
    ldp     q0,  q1,  [x0, #16]
    ldp     q2,  q3,  [x0, #48]
    ldp     q4,  q5,  [x0, #80]
    ldp     q6,  q7,  [x0, #112]
    ldp     q8,  q9,  [x0, #144]
    ldp     q10, q11, [x0, #176]
    ldp     q12, q13, [x0, #208]
    ldp     q14, q15, [x0, #240]
    ldp     q16, q17, [x0, #272]
    ldp     q18, q19, [x0, #304]
    ldp     q20, q21, [x0, #336]
    ldp     q22, q23, [x0, #368]
    ldp     q24, q25, [x0, #400]
    ldp     q26, q27, [x0, #432]
    ldp     q28, q29, [x0, #464]
    ldp     q30, q31, [x0, #496]
    ldp     x1, x2, [x0, #0]
    msr     fpcr, x1
    msr     fpsr, x2
    ret
