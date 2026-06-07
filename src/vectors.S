// Exception vector table for EL1 + register save/restore.
// The table must be 2 KiB-aligned (VBAR_EL1 requires bits [10:0] == 0).
// Each of the 16 entries occupies a 128-byte (.align 7) slot.

.section ".text"

// Frame size: 31 GPRs + elr + spsr = 33 * 8 = 264, padded to 272 (16-aligned).
.macro kernel_entry
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    mrs     x0, elr_el1            // x0/x1 already saved above; reuse them
    mrs     x1, spsr_el1
    stp     x30, x0,  [sp, #16 * 15]   // x30 @240, elr @248
    str     x1,       [sp, #16 * 16]   // spsr @256
.endm

.macro kernel_exit
    ldr     x0,       [sp, #16 * 16]   // spsr
    msr     spsr_el1, x0
    ldp     x30, x0,  [sp, #16 * 15]   // x30, elr->x0
    msr     elr_el1, x0
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    add     sp, sp, #272
    eret
.endm

.macro vector_entry label
    .align 7                  // each entry on a 128-byte boundary
    b       \label
.endm

    .align 11                 // table itself 2 KiB-aligned
    .global vector_table
vector_table:
    vector_entry default_handler   // +0x000 Sync   current EL, SP0
    vector_entry default_handler   // +0x080 IRQ    current EL, SP0
    vector_entry default_handler   // +0x100 FIQ    current EL, SP0
    vector_entry default_handler   // +0x180 SError current EL, SP0
    vector_entry el1_sync          // +0x200 Sync   current EL, SPx
    vector_entry el1_irq           // +0x280 IRQ    current EL, SPx
    vector_entry default_handler   // +0x300 FIQ    current EL, SPx
    vector_entry default_handler   // +0x380 SError current EL, SPx
    vector_entry default_handler   // +0x400 Sync   lower EL, AArch64
    vector_entry default_handler   // +0x480 IRQ    lower EL, AArch64
    vector_entry default_handler   // +0x500 FIQ    lower EL, AArch64
    vector_entry default_handler   // +0x580 SError lower EL, AArch64
    vector_entry default_handler   // +0x600 Sync   lower EL, AArch32
    vector_entry default_handler   // +0x680 IRQ    lower EL, AArch32
    vector_entry default_handler   // +0x700 FIQ    lower EL, AArch32
    vector_entry default_handler   // +0x780 SError lower EL, AArch32

el1_sync:
    kernel_entry
    mov     x0, sp
    bl      sync_handler
    kernel_exit

el1_irq:
    kernel_entry
    mov     x0, sp
    bl      irq_handler
    kernel_exit

default_handler:
    kernel_entry
    mov     x0, sp
    bl      unhandled_exception
    kernel_exit
