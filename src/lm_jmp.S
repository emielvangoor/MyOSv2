// lm_jmp.S -- a tiny freestanding setjmp/longjmp for the Lisp evaluator.
// =====================================================================
//
// The host interpreter used libc setjmp/longjmp to unwind the deeply-recursive
// evaluator back to the REPL prompt on an error (or on Ctrl-C). MyOSv2 is
// freestanding, so we provide our own. On AArch64 the only state a non-local
// jump must preserve is the callee-saved set: x19-x28, the frame pointer (x29),
// the link register (x30), and the stack pointer.
//
// A happy side effect: lm_setjmp spills every callee-saved register into the
// jmp_buf. The conservative garbage collector scans that same buffer, so any
// live Lisp object that happened to live only in a register becomes a findable
// root -- no register can hide a pointer from the collector.
//
// Layout of lm_jmp_buf (long[16]): [0..9]=x19..x28, [10]=x29, [11]=x30, [12]=sp.

.text

.global lm_setjmp
.type lm_setjmp, %function
lm_setjmp:                       // x0 = jmp_buf
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    mov x0, #0                   // setjmp returns 0 on the direct call
    ret

.global lm_longjmp
.type lm_longjmp, %function
lm_longjmp:                      // x0 = jmp_buf, x1 = value
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x9, [x0, #96]
    mov sp, x9
    cmp x1, #0                   // longjmp(buf,0) must surface as 1
    csinc x0, x1, xzr, ne
    ret
