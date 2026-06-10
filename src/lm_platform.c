/*
 * lm_platform.c -- the KERNEL platform layer for the Lisp core.
 * ===========================================================
 *
 * The portable core in lm_core.c calls a handful of hooks for memory and I/O.
 * In the kernel build (where the in-kernel KTEST suite exercises the reader,
 * evaluator and collector) those hooks map onto the kernel heap and the UART.
 * The user build (/bin/lisp) supplies its own copies of these same symbols over
 * syscalls -- see user/lm.c. Because the two are linked into separate binaries
 * there is no clash.
 */

#include "lm.h"
#include "kheap.h"
#include "uart.h"

void *lm_alloc(size_t size)
{
    char *p = kmalloc(size);
    if (p) { for (size_t i = 0; i < size; i++) { p[i] = 0; } }  /* zero, like calloc */
    return p;
}

void lm_free(void *p) { kfree(p); }

/* The KTEST build drives the core with in-memory string Readers and capture
 * Writers, so fd-backed I/O is only a courtesy: reads see EOF, writes go to the
 * UART (so a stray (print ...) during a test is at least visible). */
long lm_sys_read(int fd, void *buf, long n) { (void)fd; (void)buf; (void)n; return 0; }

long lm_sys_write(int fd, const void *buf, long n)
{
    (void)fd;
    const char *c = buf;
    for (long i = 0; i < n; i++) { uart_putc(c[i]); }
    return n;
}

long lm_open(const char *path) { (void)path; return -1; }  /* `load` untested in-kernel */
void lm_close(int fd) { (void)fd; }

/* Reached only if lm_error fires with no recovery point set. The tests always
 * evaluate through lm_eval_cstr (which installs one), so this is inert there. */
void lm_abort(void) { }
