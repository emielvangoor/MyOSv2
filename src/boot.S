// The very first code that runs. QEMU starts ALL cpus at _start; we keep only
// core 0 and park the rest, set up a stack, zero .bss, then jump to C.

.section ".text.boot"
.global _start

_start:
    // Which core am I? Low 8 bits of MPIDR_EL1 are the cpu id on virt.
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    cbz     x1, core0          // core 0 -> continue; others fall through to park

park:
    wfe                        // sleep until an event, then loop (low power)
    b       park

core0:
    // Set the stack pointer to the top of our reserved stack region.
    ldr     x1, =stack_top
    mov     sp, x1

    // Zero the .bss section (C expects uninitialized globals to be 0).
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:
    cmp     x1, x2
    b.ge    2f
    str     xzr, [x1], #8      // store 0, post-increment by 8 bytes
    b       1b
2:
    bl      kmain              // enter C

hang:
    wfe                        // if kmain ever returns, halt forever
    b       hang
