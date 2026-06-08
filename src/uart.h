// uart.h -- public interface of the serial-port (UART) driver.
// The UART is our only output device so far: writing bytes to it makes text
// appear in the terminal running QEMU.
#pragma once

void uart_init(void);        // prepare the UART for use
void uart_putc(char c);      // send one character
void uart_puts(const char *s); // send a NUL-terminated string
int  uart_getc(void);        // receive one character, or -1 if none waiting
