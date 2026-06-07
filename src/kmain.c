#include <stdint.h>
#include "uart.h"
#include "kprintf.h"
#include "exceptions.h"

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
    kprintf("Running at exception level EL%d.\n", (int)current_el());

    exc_init();
    kprintf("Vector table installed.\n");

    kprintf("Triggering a deliberate undefined instruction...\n");
    __asm__ volatile(".inst 0x00000000");   // udf #0 -> synchronous exception
    kprintf("Recovered from the fault, kernel still alive.\n");

    for (;;) {
        __asm__ volatile("wfe");
    }
}
