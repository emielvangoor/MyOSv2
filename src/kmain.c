#include "uart.h"
#include "kprintf.h"

void kmain(void)
{
    uart_init();
    kprintf("Hello, world from kernel!\n");
    kprintf("Built for ARM64 / QEMU virt. Numbers work: %d, hex %x.\n", 42, 0xcafe);
    for (;;) {
        __asm__ volatile("wfe");
    }
}
