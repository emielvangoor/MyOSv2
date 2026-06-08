// kprintf.h -- a tiny printf for the kernel, writing to the UART.
// Supports: %s %d %u %x %c %p %% and the 64-bit forms %lx %ld %lu.
#pragma once

void kprintf(const char *fmt, ...);
