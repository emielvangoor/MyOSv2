#include <stdint.h>
#include "uart.h"
#include "kprintf.h"

// Read the current exception level from the CurrentEL system register.
// Bits [3:2] hold the EL number (0=user .. 3=firmware); higher = more privileged.
static uint64_t current_el(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (v >> 2) & 3;
}

void kmain(void)
{
    uart_init();
    kprintf("Hello, world from kernel!\n");
    kprintf("Built for ARM64 / QEMU virt. Numbers work: %d, hex %x.\n", 42, 0xcafe);
    kprintf("Running at exception level EL%d.\n", (int)current_el());
    for (;;) {
        __asm__ volatile("wfe");
    }
}
