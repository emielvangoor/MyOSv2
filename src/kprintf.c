#include <stdint.h>
#include <stdarg.h>
#include "uart.h"
#include "kprintf.h"

static void print_udec(uint64_t v)
{
    char buf[20];
    int i = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

static void print_dec(int64_t v)
{
    if (v < 0) {
        uart_putc('-');
        print_udec((uint64_t)(-v));
    } else {
        print_udec((uint64_t)v);
    }
}

static void print_hex(uint64_t v)
{
    const char *digits = "0123456789abcdef";
    char buf[16];
    int i = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = digits[v & 0xf];
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

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (*p == '\n') {
                uart_putc('\r');
            }
            uart_putc(*p);
            continue;
        }
        p++; // consume '%'
        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            while (*s) {
                if (*s == '\n') {
                    uart_putc('\r');
                }
                uart_putc(*s++);
            }
            break;
        }
        case 'd':
            print_dec(va_arg(ap, int));
            break;
        case 'u':
            print_udec(va_arg(ap, unsigned int));
            break;
        case 'x':
            print_hex(va_arg(ap, unsigned int));
            break;
        case 'p':
        case 'l': // treat %l* as 64-bit; minimal handling
            if (*p == 'l') {
                p++;
                if (*p == 'x') {
                    print_hex(va_arg(ap, uint64_t));
                } else if (*p == 'd') {
                    print_dec(va_arg(ap, int64_t));
                } else if (*p == 'u') {
                    print_udec(va_arg(ap, uint64_t));
                }
            } else { // %p
                uart_putc('0');
                uart_putc('x');
                print_hex(va_arg(ap, uint64_t));
            }
            break;
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case '%':
            uart_putc('%');
            break;
        default:
            uart_putc('%');
            uart_putc(*p);
            break;
        }
    }

    va_end(ap);
}
