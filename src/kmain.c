#include "uart.h"

void kmain(void)
{
    uart_init();
    uart_puts("UART online.\n");
    for (;;) {
        __asm__ volatile("wfe");
    }
}
