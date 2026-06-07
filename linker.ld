/* Entry symbol the loader/debugger uses */
ENTRY(_start)

SECTIONS
{
    /* QEMU 'virt' RAM begins at 0x40000000; -kernel images load at 0x40080000. */
    . = 0x40080000;

    .text : {
        KEEP(*(.text.boot))   /* boot.S must come first */
        *(.text*)
    }

    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss*)
        *(COMMON)
    }
    . = ALIGN(16);
    __bss_end = .;

    /* 16 KiB stack, growing down from stack_top */
    . = ALIGN(16);
    . = . + 0x4000;
    stack_top = .;
}
