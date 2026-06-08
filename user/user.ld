/* user.ld -- link a user program as a flat binary at USER_CODE_VA. */
ENTRY(_start)
SECTIONS {
    . = 0x8000000000;          /* must match USER_CODE_VA in src/vm.h */
    .text   : { KEEP(*(.text._start)) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
}
