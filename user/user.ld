/* user.ld -- link a user program as an ELF64 executable.
 * Code/rodata go in one RX load segment; a page-aligned .data/.bss go in a
 * separate RW load segment, so the kernel's ELF loader can map each with the
 * right permissions. */
ENTRY(_start)
SECTIONS {
    . = 0x8000000000;          /* base virtual address (in the user AS region) */
    .text   : { KEEP(*(.text._start)) *(.text*) }
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);           /* start writable data on a fresh page */
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
}
