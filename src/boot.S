// boot.S -- the first code that runs on the bare machine.
// ========================================================
//
// QEMU loads our ELF and starts executing at _start with the MMU off, caches
// off, and NO stack set up. We can't run C yet (C needs a stack for locals,
// return addresses, and saved registers). This file does the minimum to create
// a sane environment, then hands control to C (kmain).
//
// AArch64 assembly quick reference for this file:
//   mrs Xd, sysreg   -> read a system register into Xd
//   x0..x30          -> 64-bit general-purpose registers
//   sp               -> stack pointer
//   bl label         -> "branch with link": call, saving return addr in x30
//   b  label         -> plain branch (jump)
//   wfe              -> "wait for event": low-power sleep until woken

// This goes in the .text.boot section, which the linker script places FIRST,
// so _start ends up at the very start of the image (0x40080000).
.section ".text.boot"
.global _start

_start:
    // ---- Step 1: keep only CPU core 0 ----
    // QEMU starts ALL cpu cores running this same code at once. If every core
    // set up the same stack and called kmain, they'd corrupt each other. So we
    // read our core id and park every core except core 0.
    //
    // MPIDR_EL1 is the "Multiprocessor Affinity Register"; its low 8 bits are
    // this core's id on the virt board.
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff          // isolate the core id
    cbz     x1, core0              // if id == 0, jump to core0; else fall through

park:
    wfe                            // secondary cores: sleep here forever
    b       park

core0:
    // ---- Step 2: set up the stack ----
    // stack_top is defined by the linker script (top of our 16 KiB region).
    // The stack grows downward, so we point sp at the high end.
    ldr     x1, =stack_top
    mov     sp, x1

    // ---- Step 3: zero the .bss section ----
    // C assumes uninitialized globals start at 0, but nobody has cleared that
    // memory for us. Loop from __bss_start to __bss_end writing zeros 8 bytes
    // at a time. (xzr is the "zero register": reads as 0.)
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:                                 // local numeric label (a loop start)
    cmp     x1, x2
    b.ge    2f                     // if x1 >= x2, we're done -> forward to label 2
    str     xzr, [x1], #8          // store 0 at [x1], then x1 += 8 (post-index)
    b       1b                     // back to label 1
2:

    // ---- Step 4: enter C ----
    bl      kmain                  // call kmain(); it normally never returns

hang:
    // Safety net: if kmain ever returns, halt instead of running off into
    // whatever bytes follow in memory.
    wfe
    b       hang
