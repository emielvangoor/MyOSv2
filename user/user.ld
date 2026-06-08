/* user.ld -- link a user program as an ELF64 executable with SEPARATE segments:
 * a read-only/executable text segment and a read-write (non-exec) data segment.
 * Keeping them in distinct PT_LOAD segments is what lets the kernel map code RO
 * (so fork's copy-on-write never demotes executable code) and data RW. */
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}
SECTIONS {
    . = 0x8000000000;                       /* base virtual address */
    .text   : { KEEP(*(.text._start)) *(.text*) } :text
    .rodata : { *(.rodata*) }               :text
    . = ALIGN(4096);                        /* writable data on its own page(s) */
    .data   : { *(.data*) }                 :data
    .bss    : { *(.bss*) *(COMMON) }        :data
}
