/*
 * linker.ld -- the kernel's memory map.
 * =====================================
 *
 * A linker script tells the linker (ld) two things:
 *   1. At what address our code/data will live in memory.
 *   2. How to group the compiled pieces (.o files) into output sections.
 *
 * On a normal OS the linker uses a default script and the OS loader handles
 * addresses. On bare metal there is no loader doing that for us, so WE must
 * state exactly where things go -- and it must match where QEMU actually
 * places the kernel, or the CPU will jump into nothing.
 */

/* The symbol the CPU/debugger treats as the program's entry point. Defined in
 * boot.S. (QEMU actually jumps to the start of the loaded image, which is also
 * _start because of the section ordering below.) */
ENTRY(_start)

SECTIONS
{
    /*
     * "." is the *location counter* -- the current output address. Setting it
     * to 0x40080000 says "lay everything out starting here."
     *
     * Why this address? QEMU's 'virt' board has RAM beginning at 0x40000000,
     * and when you pass `-kernel <elf>` it loads the image at 0x40080000.
     * If our linked addresses didn't match the load address, every absolute
     * reference (function addresses, the stack top, page tables...) would be
     * wrong and the kernel would crash instantly.
     */
    . = 0x40080000;

    /* .text = executable code. */
    .text : {
        KEEP(*(.text.boot))   /* boot.S's code, forced FIRST so _start is the
                                 very first byte of the image. KEEP() stops the
                                 linker's dead-code removal from dropping it. */
        *(.text*)             /* all other code from every .o file */
    }

    /* The user program, kept contiguous so it can be copied into a user address
     * space and run there (PC-relative). __user_start/__user_end bound it. */
    . = ALIGN(16);
    __user_start = .;
    .user : { KEEP(*(.user)) }
    __user_end = .;

    /* .rodata = read-only data: string literals, const tables. */
    .rodata : { *(.rodata*) }

    /* .data = initialized read/write globals (their initial values live in the
     * image and are already in RAM because QEMU loads the whole image). */
    .data   : { *(.data*) }

    /*
     * .bss = zero-initialized globals. These take NO space in the image (it
     * would be wasteful to store thousands of zeros), so the loader doesn't
     * fill them -- boot.S zeroes this region by hand at startup. We export
     * start/end symbols so the assembly knows the range to clear.
     */
    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss*)
        *(COMMON)
    }
    . = ALIGN(16);
    __bss_end = .;

    /*
     * The stack. There's no OS to give us one, so we carve out 16 KiB right
     * after .bss. The stack grows DOWNWARD, so boot.S sets the stack pointer
     * to stack_top (the high end) and pushes move toward lower addresses.
     * 16-byte alignment is required by the AArch64 procedure-call standard.
     */
    . = ALIGN(16);
    . = . + 0x4000;   /* reserve 16 KiB */
    stack_top = .;

    /*
     * Everything above is the kernel image. __kernel_end marks the first byte
     * AFTER it: the physical RAM from here to the end of RAM is free for the
     * page allocator (pmm.c) to manage. Page-align it so pages start clean.
     */
    . = ALIGN(4096);
    __kernel_end = .;
}
