// vectors.S -- the EL1 exception vector table and register save/restore.
// ======================================================================
//
// When an exception occurs the CPU jumps to an address computed from VBAR_EL1
// (the Vector Base Address Register) plus a fixed offset that encodes WHAT
// happened and WHERE it came from. The table has 16 slots, each 128 bytes:
//
//   group (by source/stack)        offset   types in the group
//   -----------------------        ------   ------------------
//   current EL, using SP_EL0       0x000    Sync / IRQ / FIQ / SError
//   current EL, using SP_ELx       0x200    Sync / IRQ / FIQ / SError  <- us
//   lower EL, AArch64 (EL0)        0x400    ...                        (Phase 6)
//   lower EL, AArch32              0x600    ...
//
// We currently run at EL1 using SP_EL1, so our faults land at 0x200 (Sync) and
// our timer IRQs at 0x280.
//
// A 128-byte slot is too small for a real handler, so each slot just branches
// to a routine that (1) saves all registers, (2) calls a C handler, (3)
// restores registers, (4) returns from the exception with `eret`.

.section ".text"

// ---------------------------------------------------------------------------
// kernel_entry: save the interrupted context onto the stack as a `struct
// trapframe`. We must preserve EVERY register because the interrupted code has
// no idea it was interrupted and expects all its registers intact afterward.
//
// Frame = 31 GPRs + ELR + SPSR = 33 * 8 = 264 bytes, rounded up to 272 to keep
// the stack 16-byte aligned (an AArch64 requirement). stp = "store pair".
// ---------------------------------------------------------------------------
.macro kernel_entry
    sub     sp, sp, #272               // make room for the trap frame
    stp     x0,  x1,  [sp, #16 * 0]    // save x0,x1 at offset 0
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
    mrs     x0, elr_el1                // x0/x1 are already saved above, so we
    mrs     x1, spsr_el1               //   can reuse them to grab ELR and SPSR
    stp     x30, x0,  [sp, #16 * 15]   // save x30 @240 and ELR @248
    str     x1,       [sp, #16 * 16]   // save SPSR @256
.endm

// ---------------------------------------------------------------------------
// kernel_exit: the exact reverse. Restore ELR/SPSR (so `eret` returns to the
// right place with the right flags), restore all GPRs, pop the frame, eret.
// `eret` = "exception return": resumes the interrupted code at ELR_EL1.
// ---------------------------------------------------------------------------
.macro kernel_exit
    ldr     x0,       [sp, #16 * 16]   // SPSR
    msr     spsr_el1, x0
    ldp     x30, x0,  [sp, #16 * 15]   // x30, and ELR into x0
    msr     elr_el1, x0
    ldp     x0,  x1,  [sp, #16 * 0]    // now restore the GPRs (x0,x1 last-saved
    ldp     x2,  x3,  [sp, #16 * 1]    //   value re-loaded here)
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
    add     sp, sp, #272               // pop the trap frame
    eret                               // return from exception
.endm

// One vector-table slot: align to 128 bytes, then branch to a handler routine.
.macro vector_entry label
    .align 7                  // 2^7 = 128-byte alignment for this slot
    b       \label
.endm

    .align 11                 // 2^11 = 2 KiB: VBAR_EL1 requires this alignment
    .global vector_table
vector_table:
    vector_entry default_handler   // +0x000 Sync   current EL, SP0
    vector_entry default_handler   // +0x080 IRQ    current EL, SP0
    vector_entry default_handler   // +0x100 FIQ    current EL, SP0
    vector_entry default_handler   // +0x180 SError current EL, SP0
    vector_entry el1_sync          // +0x200 Sync   current EL, SPx  <- faults
    vector_entry el1_irq           // +0x280 IRQ    current EL, SPx  <- timer
    vector_entry default_handler   // +0x300 FIQ    current EL, SPx
    vector_entry default_handler   // +0x380 SError current EL, SPx
    vector_entry default_handler   // +0x400 Sync   lower EL, AArch64 (Phase 6)
    vector_entry default_handler   // +0x480 IRQ    lower EL, AArch64
    vector_entry default_handler   // +0x500 FIQ    lower EL, AArch64
    vector_entry default_handler   // +0x580 SError lower EL, AArch64
    vector_entry default_handler   // +0x600 Sync   lower EL, AArch32
    vector_entry default_handler   // +0x680 IRQ    lower EL, AArch32
    vector_entry default_handler   // +0x700 FIQ    lower EL, AArch32
    vector_entry default_handler   // +0x780 SError lower EL, AArch32

// Each routine: save -> pass sp (the trap-frame pointer) in x0 -> call C -> restore.
el1_sync:
    kernel_entry
    mov     x0, sp                // x0 = &trapframe (first C argument)
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
