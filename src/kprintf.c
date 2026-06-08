// kprintf.c -- a minimal printf implementation for the kernel.
// ============================================================
//
// We have no C standard library (we compile with -nostdlib), so there is no
// real printf. We build a small one ourselves on top of uart_putc(). It walks
// the format string and, for each %-conversion, pulls the matching argument
// from the variadic list and prints it digit by digit.
//
// The variadic arguments are accessed with <stdarg.h>:
//   va_list ap;            -- a cursor over the extra arguments
//   va_start(ap, fmt);     -- begin (just after the named 'fmt' argument)
//   va_arg(ap, TYPE);      -- read the next argument as TYPE, advance cursor
//   va_end(ap);            -- finish

#include <stdint.h>
#include <stdarg.h>
#include "uart.h"
#include "kprintf.h"

// Print an unsigned integer in base 10.
// Trick: we generate digits least-significant first (v % 10), store them in a
// buffer, then emit the buffer in reverse so they come out in the right order.
static void print_udec(uint64_t v)
{
    char buf[20];          // 2^64 has 20 decimal digits, so this always fits
    int i = 0;
    if (v == 0) {          // the loop below prints nothing for 0, so special-case
        uart_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));  // last digit as an ASCII char
        v /= 10;
    }
    while (i > 0) {
        uart_putc(buf[--i]);                // emit in reverse = correct order
    }
}

// Print a signed integer in base 10 (handle the minus sign, then reuse udec).
static void print_dec(int64_t v)
{
    if (v < 0) {
        uart_putc('-');
        print_udec((uint64_t)(-v));
    } else {
        print_udec((uint64_t)v);
    }
}

// Print an unsigned integer in base 16 (hex). Same reverse-buffer trick, but
// each digit is 4 bits (v & 0xf) and we shift right by 4 each step.
static void print_hex(uint64_t v)
{
    const char *digits = "0123456789abcdef";
    char buf[16];          // 2^64 has 16 hex digits
    int i = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = digits[v & 0xf];   // lowest nibble -> hex char
        v >>= 4;
    }
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    // Walk the format string one character at a time.
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            // Ordinary character: print it (translating '\n' to "\r\n").
            if (*p == '\n') {
                uart_putc('\r');
            }
            uart_putc(*p);
            continue;
        }

        p++;  // we saw '%'; advance to the conversion specifier
        switch (*p) {
        case 's': {  // string
            const char *s = va_arg(ap, const char *);
            while (*s) {
                if (*s == '\n') {
                    uart_putc('\r');
                }
                uart_putc(*s++);
            }
            break;
        }
        case 'd':  // signed 32-bit decimal
            print_dec(va_arg(ap, int));
            break;
        case 'u':  // unsigned 32-bit decimal
            print_udec(va_arg(ap, unsigned int));
            break;
        case 'x':  // unsigned 32-bit hex
            print_hex(va_arg(ap, unsigned int));
            break;
        case 'p':  // pointer (printed as 0x + 64-bit hex)
        case 'l':  // 'l' length modifier -> the value is 64-bit (e.g. %lx)
            if (*p == 'l') {
                p++;  // consume the letter after 'l'
                if (*p == 'x') {
                    print_hex(va_arg(ap, uint64_t));
                } else if (*p == 'd') {
                    print_dec(va_arg(ap, int64_t));
                } else if (*p == 'u') {
                    print_udec(va_arg(ap, uint64_t));
                }
            } else {  // %p
                uart_putc('0');
                uart_putc('x');
                print_hex(va_arg(ap, uint64_t));
            }
            break;
        case 'c':  // single character
            uart_putc((char)va_arg(ap, int));
            break;
        case '%':  // a literal percent sign ("%%")
            uart_putc('%');
            break;
        default:   // unknown specifier: print it verbatim so nothing is lost
            uart_putc('%');
            uart_putc(*p);
            break;
        }
    }

    va_end(ap);
}
