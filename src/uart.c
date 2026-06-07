#include <stdint.h>
#include "uart.h"

// PL011 UART on the QEMU 'virt' board lives at this MMIO base.
#define UART0_BASE 0x09000000UL

// Register offsets (ARM PrimeCell PL011).
#define UART_DR (*(volatile uint32_t *)(UART0_BASE + 0x00)) // data register
#define UART_FR (*(volatile uint32_t *)(UART0_BASE + 0x18)) // flag register

#define UART_FR_TXFF (1u << 5) // transmit FIFO full

void uart_init(void)
{
    // QEMU's PL011 is already enabled for output at reset, so there is
    // nothing to configure yet. Real hardware would set the baud rate here.
}

void uart_putc(char c)
{
    while (UART_FR & UART_FR_TXFF) {
        // busy-wait until the transmit FIFO has room
    }
    UART_DR = (uint32_t)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r'); // terminals expect CR before LF
        }
        uart_putc(*s++);
    }
}
