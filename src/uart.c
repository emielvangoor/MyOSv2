// uart.c -- driver for the PL011 UART (serial port) on the QEMU 'virt' board.
// ===========================================================================
//
// This is our first DEVICE DRIVER. The key idea is Memory-Mapped I/O (MMIO):
// the hardware exposes its control/data registers AS MEMORY ADDRESSES. Reading
// or writing those special addresses doesn't touch RAM -- it talks to the
// device. The PL011 is the ARM "PrimeCell" UART; QEMU emulates one at a fixed
// address on the virt board.

#include <stdint.h>
#include "uart.h"

// Base address of UART0's register block on the virt board.
#define UART0_BASE 0x09000000UL

// Two registers we use, expressed as dereferenceable volatile pointers.
//
// `volatile` is ESSENTIAL: it tells the compiler these reads/writes have
// side effects on hardware and must NOT be optimized away, reordered, or
// cached in a CPU register. Without it the compiler might "helpfully" delete
// our writes (they look pointless -- we never read the value back).
#define UART_DR (*(volatile uint32_t *)(UART0_BASE + 0x00)) // Data Register
#define UART_FR (*(volatile uint32_t *)(UART0_BASE + 0x18)) // Flag Register

// Bit 5 of the Flag Register: "transmit FIFO full". While set, the UART has no
// room for another byte, so we must wait.
#define UART_FR_TXFF (1u << 5)
#define UART_FR_RXFE (1u << 4)   // receive FIFO empty

void uart_init(void)
{
    // QEMU's PL011 already comes up enabled and ready to transmit, so there is
    // nothing to configure for output. On real hardware this is where you'd set
    // the baud rate, word length, enable the FIFO, etc.
}

void uart_putc(char c)
{
    // Busy-wait until the transmit FIFO has space, then write the byte.
    while (UART_FR & UART_FR_TXFF) {
        // spin
    }
    UART_DR = (uint32_t)c;   // writing the Data Register transmits the byte
}

// Receive one character if the UART has one waiting; otherwise return -1.
int uart_getc(void)
{
    if (UART_FR & UART_FR_RXFE) {
        return -1;                 // nothing waiting
    }
    return (int)(UART_DR & 0xFF);
}

void uart_puts(const char *s)
{
    while (*s) {
        // Terminals expect a carriage-return + line-feed pair to start a new
        // line. C strings only contain '\n', so we emit '\r' first.
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}
