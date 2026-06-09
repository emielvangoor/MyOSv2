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
#define UART_DR   (*(volatile uint32_t *)(UART0_BASE + 0x00)) // Data Register
#define UART_FR   (*(volatile uint32_t *)(UART0_BASE + 0x18)) // Flag Register
#define UART_LCRH (*(volatile uint32_t *)(UART0_BASE + 0x2C)) // Line Control
#define UART_CR   (*(volatile uint32_t *)(UART0_BASE + 0x30)) // Control
#define UART_IFLS (*(volatile uint32_t *)(UART0_BASE + 0x34)) // Interrupt FIFO Level Select
#define UART_IMSC (*(volatile uint32_t *)(UART0_BASE + 0x38)) // Interrupt Mask Set/Clear
#define UART_ICR  (*(volatile uint32_t *)(UART0_BASE + 0x44)) // Interrupt Clear

// Bit 5 of the Flag Register: "transmit FIFO full". While set, the UART has no
// room for another byte, so we must wait.
#define UART_FR_TXFF (1u << 5)
#define UART_FR_RXFE (1u << 4)   // receive FIFO empty
#define UART_INT_RX  (1u << 4)   // receive interrupt
#define UART_INT_RT  (1u << 6)   // receive-timeout interrupt

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
// (uart_rx_raw is the same thing under the name the interrupt handler uses.)
int uart_getc(void)
{
    if (UART_FR & UART_FR_RXFE) {
        return -1;                 // nothing waiting
    }
    return (int)(UART_DR & 0xFF);
}
int uart_rx_raw(void) { return uart_getc(); }

// Turn on the receive interrupt. Two RX interrupt sources work together so we
// never lose a byte and never wait forever for one:
//
//   * The 16-deep RX FIFO is ENABLED (FEN=1). It absorbs bursts in hardware --
//     a pasted line, or a terminal that ships the whole line on Enter, can land
//     several bytes back-to-back faster than we service interrupts. Without the
//     FIFO (a single 1-byte holding register) such a burst yields exactly one
//     interrupt and the rest are silently dropped: the device re-asserts in the
//     drain/ack/EOI window and QEMU's PL011 never raises a fresh edge, so the
//     line stalls after the first byte or two. The FIFO removes that race.
//
//   * RXIM fires once the FIFO reaches its trigger level. We pick the LOWEST
//     level (1/8, i.e. 2 bytes) via IFLS so input is delivered promptly.
//
//   * RTIM, the receive-timeout interrupt, covers the tail: a lone byte (or the
//     final 1 below the trigger) that would otherwise sit in the FIFO with no
//     interrupt. QEMU's PL011 *does* implement it, so single keystrokes arrive.
//
// This all relies on the reader BLOCKING (never polling the data register), so
// nothing drains a byte out from under the interrupt before it is delivered.
void uart_rx_irq_enable(void)
{
    UART_IFLS  = 0;                              // RX/TX FIFO trigger at 1/8 (earliest)
    UART_LCRH |= (1u << 4);                      // FEN = 1: enable the 16-deep FIFO
    UART_ICR   = UART_INT_RX | UART_INT_RT;      // clear any stale state
    UART_IMSC |= UART_INT_RX | UART_INT_RT;      // unmask RX + receive-timeout
    UART_CR   |= (1u << 0) | (1u << 8) | (1u << 9);  // UARTEN | TXE | RXE
}

// Clear ALL interrupt sources (including any receive-overrun) so a transient
// error can't latch and stall further receive interrupts.
void uart_irq_ack(void) { UART_ICR = 0x7FF; }

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
